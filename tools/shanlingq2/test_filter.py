#!/usr/bin/env python3
"""Host check: python3 tools/shanlingq2/test_filter.py (requires cc and ALSA headers)."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
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
#define SYS_VOLUME_CHANGED MAKE_SYS_EVENT(SYS_EVENT_CLS_MISC, 5)
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
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
#include <pthread.h>
#include <stdarg.h>
#include <string.h>
static void *dac_thread(void *); /* the one thread the tests run for real */
/* no headset thread; the DAC thread for real */
static int mock_pthread_create(pthread_t *t, const pthread_attr_t *attr,
                               void *(*f)(void *), void *arg)
{
    if (f == dac_thread)
        return pthread_create(t, attr, f, arg);
    (void)t; (void)attr; (void)arg;
    return 0;
}
#define pthread_create mock_pthread_create
#define open mock_open
#define close mock_close
#define ioctl mock_ioctl
#include "firmware/target/hosted/shanling/shanlingq2_codec.c"
#include <stdlib.h>
__attribute__((noreturn)) void panicf(const char *fmt, ...)
{
    (void)fmt;
    abort();
}
static void settle(void) { assert(shanlingq2_dac_sync(2000)); }

volatile long current_tick;
int tick_add_task(void (*task)(void)) { (void)task; return 0; }
bool button_queue_empty(void) { return true; }
static int bluetooth, phones, last_filter = -1, writes;
int mock_open(const char *path, int flags, ...)
{
    (void)flags;
    assert(!strcmp(path, "/dev/shanling_dac"));
    return 42;
}
int mock_close(int fd) { assert(fd == 42); return 0; }
int mock_ioctl(int fd, unsigned long req, ...)
{
    assert(fd == 42);
    va_list args;
    va_start(args, req);
    int value = *va_arg(args, int *);
    va_end(args);
    if (req == 0xc0044d19) { last_filter = value; writes++; }
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
void pcm_alsa_set_playback_device(const char *name) { (void)name; }
void pcm_set_mixer_volume(int left, int right) { (void)left; (void)right; }
int audio_status(void) { return 0; }

int main(void)
{
    audiohw_preinit();
    settle();
    assert(last_filter == 0 && writes == 1);
    for (int i = 0; i < 4; i++) {
        audiohw_set_filter_roll_off(i);
        settle();
        assert(last_filter == i && writes == i + 2);
    }
    audiohw_set_filter_roll_off(-1);
    audiohw_set_filter_roll_off(4);
    settle();
    assert(last_filter == 3 && writes == 5);
    audiohw_close();
    bluetooth = 1;
    audiohw_preinit();
    audiohw_set_filter_roll_off(2);
    settle();
    assert(writes == 5);
    bluetooth = 0;
    audiohw_preinit();
    settle();
    assert(last_filter == 2 && writes == 6);
    audiohw_close();
    audiohw_preinit();
    settle();
    assert(last_filter == 2 && writes == 7);
    audiohw_close();
    /* headphones in a jack: the DAC, though Bluetooth would open */
    bluetooth = phones = 1;
    audiohw_preinit();
    settle();
    assert(writes == 8 && !shanlingq2_bt_output());
    audiohw_close();
}
''')
    subprocess.run(["cc", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", "-pthread", f"-I{tmp}",
                    f"-I{root}", str(tmp / "test.c"), "-o", str(tmp / "test")],
                   check=True)
    subprocess.run([str(tmp / "test")], check=True)
print("Q2 DAC filter checks passed")
