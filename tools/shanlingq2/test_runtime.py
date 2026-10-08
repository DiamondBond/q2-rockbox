#!/usr/bin/env python3
"""Native regression checks for ALSA tails/cleanup/idle and sysfs error handling."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
pcm = (root / 'firmware/target/hosted/pcm-alsa.c').read_text()


def function(name):
    # Match the definition rather than a call or mention in a comment.
    start = re.search(r'static [^\n]+\b' + name + r'\([^\n]*\)\n\{', pcm).start()
    return pcm[start:pcm.index('\n}\n', start) + 2]


source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
typedef int snd_pcm_t;
typedef long snd_pcm_sframes_t;
typedef int snd_pcm_state_t;
typedef int16_t sample_t;
#define logf(...) ((void)0)
#define WRITER_POLL_US 5000
#define SND_PCM_STATE_XRUN 1
#define SND_PCM_STATE_DISCONNECTED 2
#define SND_PCM_STATE_DRAINING 3
#define SND_PCM_STATE_SETUP 4
#define SND_PCM_STATE_PREPARED 5
static snd_pcm_t device, *handle = &device;
static sample_t *frames;
static const int channels = 2;
static snd_pcm_sframes_t period_size = 4, pending_frames, frame_offset;
static unsigned xruns;
static bool writer_run, dma_playing;
static pthread_mutex_t pcm_mtx = PTHREAD_MUTEX_INITIALIZER;
static int copied, calls, script[8], count, at, output[32], written;
static int state, start_error, reopened;
static unsigned slept;
static bool copy_frames(void) {
    for (int i = 0; i < 8; i++) frames[i] = copied * 8 + i;
    copied++;
    return true;
}
static long snd_pcm_avail_update(snd_pcm_t *p) {
    (void)p;
    return at < count ? period_size : 0;
}
static int snd_pcm_writei(snd_pcm_t *p, const sample_t *buf, long n) {
    (void)p;
    calls++;
    int result = script[at++];
    assert(result <= n);
    for (int i = 0; i < result * channels; i++) output[written++] = buf[i];
    return result;
}
static int snd_pcm_recover(snd_pcm_t *p, int err, int silent) {
    (void)p; (void)silent;
    return err == -EPIPE ? 0 : err;
}
static int snd_pcm_state(snd_pcm_t *p) { (void)p; return state; }
static int snd_pcm_start(snd_pcm_t *p) { (void)p; return start_error; }
static int writer_reopen(void) { reopened++; return -ENODEV; }
#define HAVE_HEADPHONE_DETECTION
#define DEFAULT_PLAYBACK_DEVICE "plughw:0,0"
static const char *current_alsa_device = "bluealsa";
static bool phones_in;
static int loops = 1, plug_at;
/* plugged in from the start, or once loops counts down to plug_at */
static bool headphones_inserted(void) { return phones_in || loops <= plug_at; }
#include <string.h>
static int mock_usleep(unsigned usec) { slept = usec; if (--loops <= 0) writer_run = false; return 0; }
#define usleep mock_usleep
static void close_hwdev(void) {
    /* The production cleanup must stop readers while their buffer is alive. */
    assert(frames && frames[0] == 0);
    handle = NULL;
}
'''
source += '\n'.join(function(n) for n in ('playback_fill', 'writer_main', 'alsadev_cleanup'))
source += r'''
int main(void) {
    frames = calloc(8, sizeof(*frames));
    /* One period, three writes separated by EAGAIN: no samples lost/copied twice. */
    script[0] = 1; script[1] = -EAGAIN; script[2] = 1; script[3] = 2; count = 4;
    assert(!playback_fill(handle));
    assert(copied == 1 && pending_frames == 3 && written == 2);
    assert(!playback_fill(handle));
    assert(copied == 1 && pending_frames == 0 && written == 8);
    for (int i = 0; i < 8; i++) assert(output[i] == i);
    /* A recovered underrun retries once, not endlessly, and keeps the tail. */
    at = 0; count = 1; script[0] = -EPIPE;
    assert(!playback_fill(handle) && pending_frames == 4 && xruns == 1);
    at = 0; script[0] = -ENODEV;
    assert(playback_fill(handle) == -ENODEV);
    assert(copied == 2);
    writer_run = true; dma_playing = false;
    writer_main(NULL); assert(slept == 100000 && !reopened);
    writer_run = true; dma_playing = true; state = SND_PCM_STATE_PREPARED;
    count = at = 0; start_error = -ENODEV;
    writer_main(NULL); assert(slept == 1000000 && reopened == 1);
    /* Headphones plugged in on Bluetooth: one reopen; none if already in,
     * none on the DAC. */
    start_error = 0; dma_playing = false; reopened = 0;
    writer_run = true; loops = 60;
    writer_main(NULL); assert(!reopened);
    phones_in = true; writer_run = true; loops = 60;
    writer_main(NULL); assert(!reopened);
    phones_in = false; plug_at = 30; writer_run = true; loops = 60;
    writer_main(NULL); assert(reopened == 1);
    current_alsa_device = DEFAULT_PLAYBACK_DEVICE;
    writer_run = true; loops = 60;
    writer_main(NULL); assert(reopened == 1);
    frames[0] = 0;
    alsadev_cleanup(); assert(!frames && !handle);
    return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    src, exe = tmp / 'pcm.c', tmp / 'pcm'
    src.write_text(source)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined',
                    str(src), '-pthread', '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    # Priming several periods must not replay a mixer frame: the mixer
    # prepares its next frame only on PCM_DMAST_STARTED.
    src.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
typedef int16_t sample_t;
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define panicf(...) assert(0)
enum { PCM_DMAST_OK, PCM_DMAST_STARTED };
static const long period_size = 2;
static sample_t frames[4];
static const void *pcm_data;
static size_t pcm_size;
static int16_t mix[2][4], next = 1;
static bool pcm_play_dma_complete_callback(int status, const void **addr, size_t *size) {
    (void)status; *addr = mix[next]; *size = sizeof mix[0]; return true;
}
static int pcm_play_dma_status_callback(int status) {
    static int16_t frame = 2;
    assert(status == PCM_DMAST_STARTED);
    next ^= 1;
    for (int i = 0; i < 4; i++) mix[next][i] = frame;
    frame++;
    return PCM_DMAST_OK;
}
''' + function('copy_frames') + r'''
int main(void) {
    for (int i = 0; i < 4; i++) mix[0][i] = 0, mix[1][i] = 1;
    pcm_data = mix[0]; pcm_size = sizeof mix[0];
    for (int f = 0; f < 4; f++) {
        assert(copy_frames());
        assert(frames[0] == f && frames[3] == f);
    }
    return 0;
}
''')
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                    str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    (tmp / 'config.h').write_text('')
    (tmp / 'debug.h').write_text('#define DEBUGF(...) ((void)0)\n')
    src.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sysfs.h"
int main(int argc, char **argv) {
    assert(argc == 2);
    FILE *f = fopen(argv[1], "w"); assert(f);
    fputs("invalid\n", f); fclose(f);
    int value;
    assert(!sysfs_get_int(argv[1], &value));
    assert(sysfs_set_int(argv[1], 42));
    assert(sysfs_get_int(argv[1], &value) && value == 42);
    assert(!sysfs_set_int("/dev/full", 42));
    assert(!sysfs_set_char("/dev/full", 'x'));
    assert(!sysfs_set_string("/dev/full", "text"));
    char buf[32];
    assert(!sysfs_get_string("/", buf, sizeof buf)); /* read error on a directory */
    return 0;
}
''')
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-I', str(tmp), '-I',
                    str(root / 'firmware/target/hosted'), str(src),
                    str(root / 'firmware/target/hosted/sysfs.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe), str(tmp / 'value')], check=True)
    for name in ('button.h', 'backlight.h', 'kernel.h', 'powermgmt.h'):
        (tmp / name).write_text('')
    src.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <linux/input.h>
#include <poll.h>
#define BUTTON_SELECT 1
#define BUTTON_MENU 2
#define BUTTON_PLAY 4
#define BUTTON_LEFT 8
#define BUTTON_RIGHT 16
#define BUTTON_MULTIMEDIA_VOLUME_UP 32
#define BUTTON_MULTIMEDIA_VOLUME_DOWN 64
#define BUTTON_SCROLL_FWD 128
#define BUTTON_SCROLL_BACK 256
int mock_poll(struct pollfd *, nfds_t, int);
#define poll mock_poll
#define read mock_read
#define close mock_close
static bool is_backlight_on(bool ignored) { (void)ignored; return true; }
static void backlight_on(void) {}
static void reset_poweroff_timer(void) {}
static void button_queue_post(int button, int data) { (void)button; (void)data; }
#include "firmware/target/hosted/shanling/button-q2.c"
static int mode, polled, closed;
int mock_poll(struct pollfd *p, nfds_t n, int timeout) {
    (void)timeout; assert(n == NDEV && ++polled < 4);
    p[KEYS].revents = p[WHEEL].revents = 0;
    if (mode == 0 && polled == 1) p[KEYS].revents = POLLIN;
    if (mode == 1 && p[KEYS].fd >= 0) p[KEYS].revents = POLLHUP;
    if (mode == 2 && p[WHEEL].fd >= 0) p[WHEEL].revents = POLLNVAL;
    return p[KEYS].revents || p[WHEEL].revents;
}
ssize_t mock_read(int fd, void *buf, size_t size) {
    assert(fd == 42 && size == sizeof(struct input_event));
    struct input_event e = { .type = EV_KEY, .code = KEY_DOWN, .value = 1 };
    memcpy(buf, &e, size); return size;
}
int mock_close(int fd) { assert(fd == 42 || fd == 43); closed++; return 0; }
int main(void) {
    fds[KEYS].fd = 42; fds[WHEEL].fd = 43;
    assert(button_read_device() == BUTTON_PLAY);
    mode = 1; polled = 0;
    assert(button_read_device() == 0 && fds[KEYS].fd == -1 && closed == 1);
    mode = 2; polled = 0;
    assert(button_read_device() == 0 && fds[WHEEL].fd == -1 && closed == 2);
    return 0;
}
''')
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-I', str(tmp), '-I',
                    str(root), str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('PCM tails/recovery/idle/cleanup/priming, sysfs failures and evdev disconnects: OK')
