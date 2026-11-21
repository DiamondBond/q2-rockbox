#!/usr/bin/env python3
"""Host check: python3 tools/shanlingq2/test_volume_swap.py (requires cc and ALSA headers).

The real shanlingq2_codec.c with misc.c's SYS_OUTPUT_CHANGED case: every
level the mixer plays is the output in use's own saved one, or silence."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
misc = (root / "apps/misc.c").read_text()
start = misc.index("        case SYS_OUTPUT_CHANGED:")
case = misc[start:misc.index("\n#endif", start)]

# Sound Settings > Volume (option_screen) holds the value and list position
# it opened with: it closes once misc.c has swapped the volume under it
option = (root / "apps/gui/option_select.c").read_text()
option = option[option.index("bool option_screen("):]
option = option[:option.index("\n}\n")]
handler = option.index("else if(default_event_handler(action) == SYS_USB_CONNECTED)")
assert option.index("else if (action == SYS_OUTPUT_CHANGED && variable == &global_status.volume)\n"
                    "            done = true;", handler) > handler

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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The DAC and the headset thread: accepted, never run */
static int mock_pthread_create(pthread_t *t, const pthread_attr_t *attr,
                               void *(*f)(void *), void *arg)
{
    (void)t; (void)attr; (void)f; (void)arg;
    return 0;
}
#define pthread_create mock_pthread_create
#define open mock_open
#define ioctl mock_ioctl
#include "firmware/target/hosted/shanling/shanlingq2_codec.c"
#undef pthread_create
#undef open
#undef ioctl

volatile long current_tick = 1;
int audio_status(void) { return 0; }
int tick_add_task(void (*f)(void)) { (void)f; return 0; }
int mock_open(const char *path, int flags, ...) { (void)path; (void)flags; return 42; }
int mock_ioctl(int fd, unsigned long req, ...) { (void)fd; (void)req; return 0; }

/* The outputs: wired headphones in, a headset bluez-alsa has a PCM for */
static int phones, headset;
bool headphones_inserted(void) { return phones; }
int snd_pcm_open(snd_pcm_t **pcm, const char *name,
                 snd_pcm_stream_t stream, int mode)
{
    *pcm = NULL; (void)name; (void)stream; (void)mode;
    return headset ? 0 : -1;
}
int snd_pcm_close(snd_pcm_t *pcm) { (void)pcm; return 0; }
void pcm_alsa_set_playback_device(const char *name) { (void)name; }

/* Each output's own saved level in dB: the DAC [0], the headset [1] */
static const int saved[2] = { -45, -5 };

/* The mixer: every level written, with the output in use then */
static struct { int level, bt; } mix[64];
static int mixes;
void pcm_set_mixer_volume(int left, int right)
{
    assert(left == right && mixes < 64);
    mix[mixes].level = left;
    mix[mixes++].bt = bt_output;
}

/* Rockbox's side: the button queue, global_status, sound.c and setvol */
static long queue[16];
static int queued;
bool button_queue_empty(void) { return !queued; }
static struct { int volume, parked_volume, volume_on_bt; } global_status;
void sound_set_volume(int value)
{
    global_status.volume = value;
    audiohw_set_volume(value * 10, value * 10); /* dB to tenths, as sound.c */
}
static void setvol(void) { sound_set_volume(global_status.volume); }

static int output_changed(void)
{
    switch (SYS_OUTPUT_CHANGED) {
''' + case + r'''
    }
    return -1;
}

/* One tick: button-q2's relay of the codec's event into the queue */
static void tick(void)
{
    long id;
    intptr_t data;
    current_tick++;
    while (shanlingq2_take_event(&id, &data)) {
        assert(queued < 16);
        queue[queued++] = id;
    }
}
/* The main thread takes every queued event */
static void deliver(void)
{
    for (int i = 0; i < queued; i++) {
        assert(queue[i] == SYS_OUTPUT_CHANGED);
        assert(output_changed() == 0);
    }
    queued = 0;
}
/* The writer chooses again: wired headphones win over the headset */
static void choose(int wired, int bt)
{
    phones = wired;
    headset = bt;
    audiohw_preinit();
}
/* The settings as .resume.cfg left them: volume is the flag's output's */
static void load(int on_bt)
{
    global_status.volume = saved[on_bt];
    global_status.parked_volume = saved[!on_bt];
    global_status.volume_on_bt = on_bt;
}
/* Never a level that isn't the output's own; it ends on its own, saved too */
static void check(void)
{
    for (int i = 0; i < mixes; i++)
        if (mix[i].level != -1000 && mix[i].level != saved[mix[i].bt] * 10) {
            fprintf(stderr, "mixer write %d: %d on %s, whose level is %d\n", i,
                    mix[i].level, mix[i].bt ? "Bluetooth" : "the DAC",
                    saved[mix[i].bt] * 10);
            exit(1);
        }
    assert(mixes && mix[mixes - 1].level == saved[bt_output] * 10 && !vol_pending);
    assert(global_status.volume == saved[bt_output]);
    assert(global_status.parked_volume == saved[!bt_output]);
    assert(global_status.volume_on_bt == bt_output);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *run = argv[1];
    if (!strcmp(run, "boot-wired-0") || !strcmp(run, "boot-wired-1") ||
        !strcmp(run, "boot-bt-0") || !strcmp(run, "boot-bt-1")) {
        /* pcm_init chooses, settings load (audio not up: no set), main loop */
        choose(!strncmp(run, "boot-wired", 10), 1);
        load(run[strlen(run) - 1] == '1');
        tick();
        deliver();
    } else if (!strcmp(run, "takeover")) {
        choose(1, 0);
        load(0);
        tick();
        deliver();
        choose(0, 1); /* unplugged, the headset takes over */
        tick();
        deliver();
        assert(bt_output);
        choose(1, 1); /* plugged in again */
        tick();
        deliver();
        assert(!bt_output);
    } else if (!strcmp(run, "three-queued")) {
        choose(0, 1);
        load(1);
        tick();
        deliver();
        /* wired, Bluetooth, wired, all behind one main-thread stall */
        choose(1, 1);
        tick();
        choose(0, 1);
        tick();
        choose(1, 1);
        tick();
        assert(queued == 3);
        deliver();
        assert(!bt_output);
    } else if (!strcmp(run, "dropped")) {
        choose(1, 0);
        load(0);
        tick();
        deliver();
        choose(0, 1);
        tick();
        assert(queued == 1);
        queued = 0; /* a screen cleared the queue */
        int n = 0;
        while (!queued && n++ < 2 * HZ)
            tick();
        assert(queued == 1 && n <= HZ + 1); /* offered again within a second */
        deliver();
        assert(bt_output);
    } else
        return 2;
    check();
    return 0;
}
''')
    subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                    "-pthread", f"-I{tmp}", f"-I{root}", str(tmp / "test.c"),
                    "-o", str(tmp / "test")], check=True)
    for run in ("boot-wired-0", "boot-wired-1", "boot-bt-0", "boot-bt-1",
                "takeover", "three-queued", "dropped"):
        subprocess.run([str(tmp / "test"), run], check=True, timeout=30)
print("Q2 per-output volume checks passed")
