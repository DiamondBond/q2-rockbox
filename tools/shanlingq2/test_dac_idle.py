#!/usr/bin/env python3
"""Host check: python3 tools/shanlingq2/test_dac_idle.py (requires cc and ALSA headers)."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]


def function(name):
    source = (root / "firmware/target/hosted/pcm-alsa.c").read_text()
    start = source.index("static void " + name + "(")
    return source[start:source.index("\n}\n", start)]


# The wake is sink_dma_start's first statement, the stamp sink_dma_stop's last;
# at a rate change the DAC catches up (the stop's mute) before the drop
setfreq = (root / "firmware/target/hosted/pcm-alsa.c").read_text()
setfreq = setfreq[setfreq.index("if (last_sample_rate != sampr)"):]
assert setfreq.index("#ifdef HAVE_SHANLINGQ2_CODEC\n        shanlingq2_dac_sync(") < setfreq.index("snd_pcm_drop(handle)")
assert "{\n#ifdef HAVE_SHANLINGQ2_CODEC\n    shanlingq2_dac_wake();\n#endif\n" in function("sink_dma_start")
assert function("sink_dma_stop").endswith("#ifdef HAVE_SHANLINGQ2_CODEC\n    shanlingq2_dac_idle();\n#endif")

with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    (tmp / "config.h").write_text("")
    (tmp / "audiohw.h").write_text("""
#include <stdbool.h>
#include <stdint.h>
#define SOUND_HIGH_POWER 0
#define AUDIOHW_SETTING(...)
#include "firmware/export/shanlingq2_codec.h"
""")
    (tmp / "button.h").write_text("""
bool headphones_inserted(void);
bool button_queue_empty(void);
""")
    (tmp / "kernel.h").write_text("""
#define HZ 100
#define TIME_BEFORE(a, b) ((long)(a) - (long)(b) < 0)
#define TIME_AFTER(a, b) TIME_BEFORE(b, a)
#define SYS_EVENT_CLS_MISC 5
#define MAKE_SYS_EVENT(cls, id) ((long)(1u << 31) | (long)(cls) << 28 | (id))
extern volatile long current_tick;
int tick_add_task(void (*)(void));
""")
    (tmp / "panic.h").write_text("void panicf(const char *, ...) __attribute__((noreturn));\n")
    (tmp / "pcm-alsa.h").write_text("""
void pcm_alsa_set_playback_device(const char *);
void pcm_set_mixer_volume(int, int);
""")
    (tmp / "audio.h").write_text("""
#define AUDIO_STATUS_PLAY  1
#define AUDIO_STATUS_PAUSE 2
int audio_status(void);
""")
    (tmp / "test.c").write_text(r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

static void *dac_thread(void *); /* the one thread the tests run for real */
/* dbus-send and the headset thread: counted, never run */
static int spawns, spawn_fails, sent_byte = -1, threads;
static int mock_posix_spawn(pid_t *pid, const char *path,
                            const posix_spawn_file_actions_t *fa,
                            const posix_spawnattr_t *attr,
                            char *const argv[], char *const envp[])
{
    (void)fa; (void)attr; (void)envp;
    assert(!strcmp(path, "/usr/bin/dbus-send"));
    assert(!strcmp(argv[6], "shanling.bluez.NotifyChanged"));
    assert(!strcmp(argv[7], "int32:263") && !argv[9]);
    assert(sscanf(argv[8], "array:byte:%d", &sent_byte) == 1);
    spawns++;
    if (spawn_fails)
        return ENOENT; /* no dbus-send */
    *pid = 1234;
    return 0;
}
static pid_t mock_waitpid(pid_t pid, int *status, int options)
{
    assert(pid == 1234 && !status && !options && !spawn_fails);
    return pid;
}
static int mock_pthread_create(pthread_t *t, const pthread_attr_t *attr,
                               void *(*f)(void *), void *arg)
{
    if (f == dac_thread)
        return pthread_create(t, attr, f, arg);
    threads++;
    return 0;
}
#define posix_spawn mock_posix_spawn
#define waitpid mock_waitpid
#define pthread_create mock_pthread_create
int mock_close(int); /* unistd.h may already be in: declared here */
#define open mock_open
#define close mock_close
#define ioctl mock_ioctl
#include "firmware/target/hosted/shanling/shanlingq2_codec.c"
#undef pthread_create
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
__attribute__((noreturn)) void panicf(const char *fmt, ...)
{
    (void)fmt;
    abort();
}
static void settle(void) { assert(shanlingq2_dac_sync(2000)); }

static int playing;
int audio_status(void) { return playing; }

volatile long current_tick;
static void (*task)(void);
static int tasks, bluetooth, phones, opens, mixer_calls, calls, queued;
static int mixer_l = 1, mixer_r = 1; /* the mixer's last volume, 1: never set */
static struct { unsigned long req; int value; } log_[32];
static volatile int stall_us; /* the next ioctl sleeps this long: a stuck driver */

int tick_add_task(void (*f)(void)) { task = f; tasks++; return 0; }
int mock_open(const char *path, int flags, ...)
{
    (void)flags;
    assert(!strcmp(path, "/dev/shanling_dac"));
    opens++;
    return 42;
}
static int closes;
int mock_close(int fd) { assert(fd == 42); closes++; return 0; }
int mock_ioctl(int fd, unsigned long req, ...)
{
    assert(fd == 42 && calls < 32);
    if (stall_us) {
        int us = stall_us;
        stall_us = 0;
        usleep(us);
    }
    va_list args;
    va_start(args, req);
    log_[calls].req = req;
    log_[calls++].value = *va_arg(args, int *);
    va_end(args);
    return 0;
}
int snd_pcm_open(snd_pcm_t **pcm, const char *name,
                 snd_pcm_stream_t stream, int mode)
{
    *pcm = NULL; (void)name; (void)stream; (void)mode;
    return bluetooth ? 0 : -1;
}
int snd_pcm_close(snd_pcm_t *pcm) { (void)pcm; return 0; }
bool headphones_inserted(void) { return phones; }
bool button_queue_empty(void) { return !queued; }
void pcm_alsa_set_playback_device(const char *name) { (void)name; }
void pcm_set_mixer_volume(int left, int right)
{
    mixer_l = left;
    mixer_r = right;
    mixer_calls++;
}

/* Run the tick tasks up to and including this many ticks from now */
static void ticks(long n)
{
    while (n-- > 0) {
        current_tick++;
        if (task)
            task();
    }
}
static void no_calls(void) { settle(); assert(calls == 0); }
static void expect_replay(int g, int f, int m)
{
    settle();
    assert(calls == 7);
    assert(log_[0].req == DAC_HEADSET && log_[0].value == 1);
    assert(log_[1].req == DAC_POWER && log_[1].value == 1);
    assert(log_[2].req == DAC_PCM && log_[2].value == 0);
    assert(log_[3].req == DAC_VOLUME && log_[3].value == (100 << 8 | 100));
    assert(log_[4].req == DAC_GAIN && log_[4].value == g);
    assert(log_[5].req == DAC_FILTER && log_[5].value == f);
    assert(log_[6].req == DAC_MUTE && log_[6].value == m);
    calls = 0;
}
static void expect_off(void)
{
    settle();
    assert(calls == 2);
    assert(log_[0].req == DAC_MUTE && log_[0].value == 1);
    assert(log_[1].req == DAC_POWER && log_[1].value == 0);
    calls = 0;
}
/* From a stop just now: nothing until the 30 s are up, then off, then quiet */
static void expires_at_30s(void)
{
    ticks(30 * HZ - 1);
    no_calls();
    ticks(1);
    expect_off();
    ticks(60 * HZ);
    no_calls();
}

static void *tick_thread(void *arg)
{
    (void)arg;
    task();
    return NULL;
}

int main(void)
{
    alarm(20); /* a lock taken twice dies here rather than hangs */

    /* Preinit doesn't arm, postinit does: browsing with nothing played */
    audiohw_preinit();
    expect_replay(0, 0, 0);
    /* The first choice is a change: the mixer silent, the DAC told to misc.c */
    long id;
    intptr_t data;
    assert(vol_pending && mixer_l == -1000 && mixer_r == -1000);
    assert(shanlingq2_take_event(&id, &data) && id == SYS_OUTPUT_CHANGED && data == 0);
    assert(!shanlingq2_take_event(&id, &data));
    assert(!tasks);
    ticks(60 * HZ);
    no_calls();
    audiohw_set_volume(-200, -300);
    audiohw_postinit();
    assert(tasks == 1);
    int mixed = mixer_calls;
    expires_at_30s();
    /* the expiry leaves the stored mute, the volume and the mixer alone */
    assert(muted == 0 && vol[0] == -200 && vol[1] == -300 && mixer_calls == mixed);

    /* Changes while off: stored, no ioctl, and in the one replay */
    audiohw_set_power_mode(SOUND_HIGH_POWER);
    audiohw_set_filter_roll_off(3);
    audiohw_mute(true);
    no_calls();
    assert(mixer_calls == mixed + 1); /* Rockbox's mixer still mutes */
    shanlingq2_dac_wake();
    expect_replay(1, 3, 1);
    shanlingq2_dac_wake();
    no_calls();

    /* Playing never expires; while on, changes reach the DAC */
    ticks(120 * HZ);
    no_calls();
    audiohw_mute(false);
    settle();
    assert(calls == 1 && log_[0].req == DAC_MUTE && log_[0].value == 0);
    calls = 0;
    audiohw_set_filter_roll_off(2);
    settle();
    assert(calls == 1 && log_[0].req == DAC_FILTER && log_[0].value == 2);
    calls = 0;
    audiohw_set_power_mode(1);
    settle();
    assert(calls == 1 && log_[0].req == DAC_GAIN && log_[0].value == 0);
    calls = 0;

    /* The stamp alone in the idle hook; a start before expiry cancels it */
    shanlingq2_dac_idle();
    no_calls();
    ticks(30 * HZ - 1);
    no_calls();
    shanlingq2_dac_wake();
    no_calls();
    ticks(120 * HZ);
    no_calls();

    /* A later stop restarts the 30 s, whatever came before */
    shanlingq2_dac_idle();
    ticks(10 * HZ);
    shanlingq2_dac_idle();
    expires_at_30s();
    /* Replays the mute as stored: unmuted here */
    shanlingq2_dac_wake();
    expect_replay(0, 2, 0);

    /* A stop at tick 0 still counts, about the same 30 s */
    current_tick = -1;
    ticks(1);
    assert(current_tick == 0);
    shanlingq2_dac_idle();
    assert(idle_since);
    ticks(30 * HZ);
    no_calls();
    ticks(1);
    expect_off();
    shanlingq2_dac_wake();
    expect_replay(0, 2, 0);

    /* What SIGIO reaches takes no lock, nor does an unexpired tick: these
     * would deadlock under dac_mtx */
    pthread_mutex_lock(&dac_mtx);
    shanlingq2_dac_idle();
    audiohw_mute(true);
    settle(); /* the sync takes no dac_mtx either */
    assert(calls == 1 && log_[0].req == DAC_MUTE && log_[0].value == 1);
    calls = 0;
    audiohw_mute(false);
    settle();
    calls = 0;
    ticks(10);
    no_calls();
    pthread_mutex_unlock(&dac_mtx);

    /* A tick past its compare, waiting on the lock when a wake clears the
     * stamp, must look again: no power off under a start */
    ticks(30 * HZ);
    expect_off();
    shanlingq2_dac_wake();
    expect_replay(0, 2, 0);
    shanlingq2_dac_idle();
    current_tick += 30 * HZ;
    pthread_t thread;
    pthread_mutex_lock(&dac_mtx);
    assert(!pthread_create(&thread, NULL, tick_thread, NULL));
    usleep(50000);
    idle_since = 0;
    pthread_mutex_unlock(&dac_mtx);
    assert(!pthread_join(thread, NULL));
    no_calls();

    /* Fallback during playback: the stamp stays clear, no power off */
    audiohw_close();
    calls = 0;
    bluetooth = 1;
    audiohw_preinit();
    no_calls();
    shanlingq2_dac_wake();
    no_calls();
    bluetooth = 0;
    audiohw_preinit();
    expect_replay(0, 2, 0);
    ticks(120 * HZ);
    no_calls();
    shanlingq2_dac_idle();
    expires_at_30s();
    shanlingq2_dac_wake();
    expect_replay(0, 2, 0);

    /* Bluetooth, the DAC never opened: nothing at all reaches it */
    audiohw_close();
    calls = 0;
    bluetooth = 1;
    int opened = opens;
    audiohw_preinit();
    shanlingq2_dac_idle();
    ticks(60 * HZ);
    shanlingq2_dac_wake();
    audiohw_mute(false);
    audiohw_set_power_mode(0);
    audiohw_set_filter_roll_off(1);
    ticks(60 * HZ);
    shanlingq2_dac_idle();
    audiohw_mute(true);
    ticks(60 * HZ);
    audiohw_close();
    no_calls();
    assert(opens == opened);

    /* Fallback while idle on Bluetooth: on, then off on the stamp it had */
    audiohw_preinit();
    shanlingq2_dac_idle();
    ticks(60 * HZ);
    bluetooth = 0;
    audiohw_preinit();
    expect_replay(1, 1, 0);
    ticks(1);
    expect_off();
    shanlingq2_dac_wake();
    expect_replay(1, 1, 0);

    /* A headset taking over from the DAC powers it off at once, as idle does;
     * a start stays off while Bluetooth is the output, and so does choosing it
     * again; the fallback replays it all */
    bluetooth = 1;
    audiohw_preinit();
    expect_off();
    shanlingq2_dac_idle();
    ticks(60 * HZ);
    shanlingq2_dac_wake();
    audiohw_preinit();
    no_calls();
    bluetooth = 0;
    audiohw_preinit();
    expect_replay(1, 1, 0);
    /* Off on idle, then the headset: nothing more reaches the DAC */
    shanlingq2_dac_idle();
    expires_at_30s();
    bluetooth = 1;
    audiohw_preinit();
    no_calls();
    bluetooth = 0;
    audiohw_preinit();
    expect_replay(1, 1, 0);

    /* Headphones win over a headset that would open: the DAC again. Close left
     * it muted, so preinit replays that and then unmutes. */
    audiohw_close();
    calls = 0;
    bluetooth = phones = 1;
    audiohw_preinit();
    assert(!shanlingq2_bt_output());
    settle();
    assert(calls == 8 && log_[7].req == DAC_MUTE && log_[7].value == 0);
    calls = 7;
    expect_replay(1, 1, 1);

    /* Each caller that has to wait does, with no settle: a power-on behind a
     * slow driver is made before preinit and a wake return, and close makes
     * the power-off before it closes the node */
    stall_us = 200000;
    int closed = closes;
    audiohw_close();
    assert(calls == 2 && log_[0].req == DAC_MUTE && log_[0].value == 1 &&
           log_[1].req == DAC_POWER && log_[1].value == 0 && closes == closed + 1);
    calls = 0;
    phones = 0;
    bluetooth = 0;
    stall_us = 200000;
    audiohw_preinit(); /* close left it muted: the replay, then the unmute */
    assert(calls == 8 && log_[1].req == DAC_POWER && log_[1].value == 1 &&
           log_[7].req == DAC_MUTE && log_[7].value == 0);
    calls = 0;
    shanlingq2_dac_idle();
    ticks(30 * HZ);
    stall_us = 200000;
    shanlingq2_dac_wake();
    assert(calls == 9 && log_[3].req == DAC_POWER && log_[3].value == 1);
    calls = 0;
    /* the DAC already on: a wake doesn't wait behind a stall */
    struct timespec t0, t1;
    stall_us = 1000000;
    audiohw_mute(true);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    shanlingq2_dac_wake();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    assert((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 < 500);
    settle();
    calls = 0;
    audiohw_mute(false);
    settle();
    calls = 0;

    /* A close behind a stall longer than its bound leaves the node open and
     * the queue as it is: the power-off is made once the stall ends */
    stall_us = 1500000;
    closed = closes;
    audiohw_close();
    assert(closes == closed && dac == 42);
    settle();
    assert(calls == 2 && log_[1].req == DAC_POWER && log_[1].value == 0);
    calls = 0;
    audiohw_preinit(); /* the node still open: the DAC on again */
    settle();
    calls = 0;

    /* A stalled driver: the callers return at once, the wait is bounded, and a
     * mute and an unmute sent during the stall both arrive, in order */
    stall_us = 1500000;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    audiohw_mute(true);
    audiohw_mute(false);
    assert(!shanlingq2_dac_sync(100));
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long waited = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    assert(waited >= 100 && waited < 1000);
    assert(shanlingq2_dac_sync(3000));
    assert(calls == 2 && log_[0].req == DAC_MUTE && log_[0].value == 1 &&
           log_[1].req == DAC_MUTE && log_[1].value == 0);
    calls = 0;
    /* a tick that expires behind a stall queues and returns */
    audiohw_set_power_mode(0);
    settle();
    calls = 0;
    shanlingq2_dac_idle();
    stall_us = 1000000;
    audiohw_set_filter_roll_off(1); /* stalls; the tick's power-off queues behind it */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    ticks(30 * HZ);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    assert((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 < 500);
    settle();
    assert(calls == 3 && log_[0].req == DAC_FILTER && log_[1].req == DAC_MUTE &&
           log_[2].req == DAC_POWER && log_[2].value == 0);
    calls = 0;
    shanlingq2_dac_wake();
    expect_replay(1, 1, 0); /* mode 0 is SOUND_HIGH_POWER here: gain 1 */

    /* After close the tick and a stray wake stay harmless */
    audiohw_close();
    calls = 0;
    assert(!dac_on);
    shanlingq2_dac_idle();
    ticks(60 * HZ);
    shanlingq2_dac_wake();
    assert(!dac_on);
    audiohw_set_filter_roll_off(0);
    audiohw_set_power_mode(1);
    ticks(60 * HZ);
    no_calls();
    assert(tasks == 1); /* the one postinit's */

    /* The headset's play state: none while the DAC is the output */
    int sent = 5;
    phones = bluetooth = 0;
    audiohw_preinit();
    playing = AUDIO_STATUS_PLAY;
    bt_status_check(&sent);
    assert(!spawns);
    /* one send per change, none without one; the thread started once */
    bluetooth = 1;
    audiohw_preinit();
    assert(threads == 1);
    bt_status_check(&sent);
    assert(spawns == 1 && sent_byte == 1);
    bt_status_check(&sent);
    assert(spawns == 1);
    playing = AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE;
    bt_status_check(&sent);
    bt_status_check(&sent);
    assert(spawns == 2 && sent_byte == 2);
    playing = 0;
    bt_status_check(&sent);
    bt_status_check(&sent);
    assert(spawns == 3 && sent_byte == 0);
    /* Bluetooth chosen afresh: the same state again */
    audiohw_preinit();
    bt_status_check(&sent);
    assert(spawns == 4 && sent_byte == 0 && threads == 1);
    /* a failed send is not tried again until the next change */
    spawn_fails = 1;
    playing = AUDIO_STATUS_PLAY;
    bt_status_check(&sent);
    bt_status_check(&sent);
    spawn_fails = 0;
    bt_status_check(&sent);
    assert(spawns == 5 && sent_byte == 1);
    playing = 0;
    bt_status_check(&sent);
    assert(spawns == 6 && sent_byte == 0);

    /* Each output its own volume (misc.c). The headset to the DAC: silent,
     * and the DAC told; misc.c's sets and an unmute before its ack stay silent */
    audiohw_mute(false);
    bluetooth = 0;
    current_tick += 2 * HZ;
    audiohw_preinit();
    calls = 0;
    assert(shanlingq2_take_event(&id, &data) && id == SYS_OUTPUT_CHANGED && data == 0);
    audiohw_set_volume(-400, -450);
    audiohw_mute(true);
    audiohw_mute(false);
    assert(mixer_l == -1000 && mixer_r == -1000);
    /* an ack for the headset (the change before) holds; the DAC's releases at
     * the DAC's level */
    shanlingq2_volume_ack(true);
    assert(vol_pending && mixer_l == -1000);
    shanlingq2_volume_ack(false);
    assert(!vol_pending && mixer_l == -400 && mixer_r == -450);
    audiohw_set_volume(-350, -350);
    assert(mixer_l == -350);

    /* Back to the headset; a screen drops the event. Not offered again while
     * anything is queued, nor within the second, then once a second */
    bluetooth = 1;
    audiohw_preinit();
    assert(vol_pending && mixer_l == -1000);
    audiohw_set_volume(-100, -100);
    assert(mixer_l == -1000);
    assert(shanlingq2_take_event(&id, &data) && data == 1);
    assert(!shanlingq2_take_event(&id, &data));
    queued = 1;
    ticks(3 * HZ);
    assert(!shanlingq2_take_event(&id, &data));
    queued = 0;
    assert(shanlingq2_take_event(&id, &data) && id == SYS_OUTPUT_CHANGED && data == 1);
    ticks(HZ);
    assert(!shanlingq2_take_event(&id, &data));
    ticks(1);
    assert(shanlingq2_take_event(&id, &data) && data == 1);
    shanlingq2_volume_ack(true);
    assert(!vol_pending && mixer_l == -100);
    ticks(3 * HZ);
    assert(!shanlingq2_take_event(&id, &data));

    /* The headset again (a rate change's reopen): nothing told, not silenced */
    int mixed_bt = mixer_calls;
    audiohw_preinit();
    assert(!vol_pending && mixer_calls == mixed_bt);
    assert(!shanlingq2_take_event(&id, &data));
    calls = 0;
    return 0;
}
''')
    subprocess.run(["cc", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                    "-pthread", f"-I{tmp}", f"-I{root}", str(tmp / "test.c"),
                    "-o", str(tmp / "test")], check=True)
    subprocess.run([str(tmp / "test")], check=True, timeout=30)
print("Q2 DAC idle checks passed")
