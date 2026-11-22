#!/usr/bin/env python3
"""Host check: python3 tools/shanlingq2/test_device_volume.py (requires cc and ALSA headers).

The real shanlingq2_codec.c with misc.c's SYS_OUTPUT_CHANGED and
SYS_VOLUME_CHANGED cases, BlueZ's MediaTransport1 Volume behind a stand-in
dbus-send: Control Device Volume off changes nothing; on, the headset is never
raised by a take (a switch on starts it at 0), the mixer leaves the volume to
it only once it has read back at Rockbox's level or its own step of it, only
the guard's two write-backs undo a level it reported, and a number misc.c
never set is never written to it."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
misc = (root / "apps/misc.c").read_text()
start = misc.index("        case SYS_OUTPUT_CHANGED:")
cases = misc[start:misc.index("\n#endif", start)]
assert "case SYS_VOLUME_CHANGED:" in cases

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
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

static void *dac_thread(void *); /* the one thread the tests run for real */
/* The status thread: accepted, never run (step() makes its passes); the DAC
 * thread for real */
static int mock_pthread_create(pthread_t *t, const pthread_attr_t *attr,
                               void *(*f)(void *), void *arg)
{
    if (f == dac_thread)
        return pthread_create(t, attr, f, arg);
    (void)t; (void)attr; (void)arg;
    return 0;
}
static int mock_spawn(pid_t *, const char *, const posix_spawn_file_actions_t *,
                      const posix_spawnattr_t *, char *const [], char *const []);
static int mock_adddup2(posix_spawn_file_actions_t *fa, int fd, int to);
static pid_t mock_waitpid(pid_t pid, int *status, int options);
#define pthread_create mock_pthread_create
#define open mock_open
#define ioctl mock_ioctl
#define posix_spawn mock_spawn
#define posix_spawn_file_actions_adddup2 mock_adddup2
#define waitpid mock_waitpid
#include "firmware/target/hosted/shanling/shanlingq2_codec.c"
#undef pthread_create
#undef open
#undef ioctl
#undef posix_spawn
#undef posix_spawn_file_actions_adddup2
#undef waitpid
__attribute__((noreturn)) void panicf(const char *fmt, ...)
{
    (void)fmt;
    abort();
}

volatile long current_tick = 1;
static int playing = AUDIO_STATUS_PLAY;
int audio_status(void) { return playing; }
int tick_add_task(void (*f)(void)) { (void)f; return 0; }
int mock_open(const char *path, int flags, ...) { (void)path; (void)flags; return 42; }
int mock_ioctl(int fd, unsigned long req, ...) { (void)fd; (void)req; return 0; }

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

/* The headset as BlueZ 5.66 has it: what it plays at, BlueZ's Volume (-1: none
 * yet), whether it takes writes, its own step (a write answered with the next
 * level up), how many transports, Gets left to fail, and every write */
static struct {
    int actual, bluez, ignores, step, transports, gone, fail;
    int writes[64], nwrites;
} hs;

/* dbus-send and sh: the reply on stdout, the exit status for waitpid */
static int out_fd = -1, exit_code, spawns;
static int mock_adddup2(posix_spawn_file_actions_t *fa, int fd, int to)
{
    (void)fa;
    if (to == 1)
        out_fd = fd;
    return 0;
}
static void reply(const char *s)
{
    ssize_t r = write(out_fd, s, strlen(s));
    (void)r;
}
static int mock_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *fa,
                      const posix_spawnattr_t *attr, char *const argv[], char *const env[])
{
    (void)path; (void)fa; (void)attr; (void)env;
    char line[160];
    spawns++;
    *pid = 1234;
    exit_code = 0;
    if (!strcmp(argv[0], "/bin/sh")) { /* the transports' paths, through sed */
        assert(strstr(argv[2], "GetManagedObjects"));
        for (int i = 0; !hs.gone && i < hs.transports; i++) {
            snprintf(line, sizeof line, "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF/sep1/fd%d\n", i);
            reply(line);
        }
        return 0;
    }
    int n = 0;
    while (argv[n])
        n++;
    assert(n >= 9 && !strcmp(argv[0], "/usr/bin/dbus-send") && !strcmp(argv[4], "--dest=org.bluez"));
    assert(!strcmp(argv[7], "string:org.bluez.MediaTransport1") && !strcmp(argv[8], "string:Volume"));
    if (hs.gone) {
        exit_code = 1;
        return 0;
    }
    if (!strcmp(argv[6], "org.freedesktop.DBus.Properties.Get")) {
        if (hs.fail) { /* bluetoothd slow: the reply timed out */
            hs.fail--;
            exit_code = 1;
            return 0;
        }
        if (hs.bluez < 0) { /* no such property: no absolute volume */
            exit_code = 1;
            return 0;
        }
        snprintf(line, sizeof line, "method return time=1 sender=:1.2\n   variant       uint16 %d\n", hs.bluez);
        reply(line);
        return 0;
    }
    assert(!strcmp(argv[6], "org.freedesktop.DBus.Properties.Set") && n == 10);
    int v;
    assert(sscanf(argv[9], "variant:uint16:%d", &v) == 1 && v >= 0 && v <= 127);
    assert(hs.nwrites < 64);
    hs.writes[hs.nwrites++] = v;
    if (!hs.ignores) /* its answer: BlueZ takes the level from it */
        hs.actual = hs.bluez = hs.step ? MIN(127, (v + hs.step - 1) / hs.step * hs.step) : v;
    return 0;
}
static pid_t mock_waitpid(pid_t pid, int *status, int options)
{
    (void)options;
    *status = exit_code << 8;
    return pid;
}

/* Each output's own saved level in dB: the DAC [0], the headset [1] */
static int saved[2] = { -45, -60 };
static int limit = 0;

/* The mixer: every level written, with the state it was written in */
static struct { int level, bt, ctl, actual, vol; } mix[512];
static int mixes;
void pcm_set_mixer_volume(int left, int right)
{
    assert(left == right && mixes < 512);
    mix[mixes].level = left;
    mix[mixes].bt = bt_output;
    mix[mixes].ctl = dev_ctl;
    mix[mixes].actual = hs.actual;
    mix[mixes++].vol = MAX(vol[0], vol[1]);
}

/* Rockbox's side */
static long queue[16];
static intptr_t qdata[16];
static int queued;
static intptr_t data_now;
bool button_queue_empty(void) { return !queued; }
static intptr_t button_get_data(void) { return data_now; }
static struct { int volume, parked_volume, volume_on_bt; } global_status;
void sound_set_volume(int value)
{
    if (value > limit)
        value = limit;
    global_status.volume = value;
    audiohw_set_volume(value * 10, value * 10);
}
static void setvol(void) { sound_set_volume(global_status.volume); }

static int handle(long event)
{
    switch (event) {
''' + cases + r'''
    }
    return -1;
}

/* SYS_VOLUME_CHANGED delivered, and how many of the next ones a plugin
 * swallows (button_get with no default_event_handler, or a queue cleared) */
static int volume_events, swallow;
static void deliver(void)
{
    for (int i = 0; i < queued; i++) {
        data_now = qdata[i];
        if (queue[i] == SYS_VOLUME_CHANGED) {
            volume_events++;
            if (swallow) {
                swallow--;
                continue;
            }
        }
        assert(handle(queue[i]) == 0);
    }
    queued = 0;
}
/* A quarter second: the status thread's look, the tick's relay, the main loop */
static void step(void)
{
    long id;
    intptr_t data;
    current_tick += HZ / 4;
    dev_volume_pass();
    while (shanlingq2_take_event(&id, &data)) {
        assert(queued < 16);
        queue[queued] = id;
        qdata[queued++] = data;
    }
    deliver();
}
/* The main loop before the status thread's next look */
static void main_first(void)
{
    long id;
    intptr_t data;
    while (shanlingq2_take_event(&id, &data)) {
        queue[queued] = id;
        qdata[queued++] = data;
    }
    deliver();
}
static void steps(int n)
{
    while (n--)
        step();
}
static void choose(int wired, int bt)
{
    phones = wired;
    headset = bt;
    audiohw_preinit();
}
static void load(int on_bt)
{
    global_status.volume = saved[on_bt];
    global_status.parked_volume = saved[!on_bt];
    global_status.volume_on_bt = on_bt;
}
/* Rockbox started on the headset, settings applied */
static void boot_bt(int link)
{
    choose(0, 1);
    load(1);
    shanlingq2_set_device_volume(link);
    step(); /* the swap's set ends the silence */
    assert(bt_output && !vol_pending);
    step(); /* the first look: the take's write */
}

/* Over a whole run: the mixer raised above Rockbox's own level only on the
 * headset, in control, with the headset at or below Rockbox's level (the
 * wheel turned down lowers Rockbox's level a moment before the headset's) */
static void check(void)
{
    for (int i = 0; i < mixes; i++) {
        if (mix[i].level <= mix[i].vol || (i && mix[i].level <= mix[i - 1].level))
            continue;
        if (!mix[i].bt || !mix[i].ctl ||
            dev_to_pct(mix[i].actual) > (mix[i].vol < -1000 ? 0 : mix[i].vol / 10 + 100)) {
            fprintf(stderr, "mixer write %d: %d over a volume of %d, headset at %d, %s, %s\n",
                    i, mix[i].level, mix[i].vol, mix[i].actual,
                    mix[i].bt ? "Bluetooth" : "the DAC", mix[i].ctl ? "in control" : "not");
            exit(1);
        }
    }
}
static int last_mix(void) { return mix[mixes - 1].level; }
/* Q2 Pod left it at 127; Rockbox's headset level is -60 dB, 40 %: in control */
static void handover(void)
{
    hs.actual = hs.bluez = 127;
    boot_bt(1);
    steps(8);
    assert(dev_ctl && hs.actual == 51);
}
/* A headset that holds to its own raise in the first 10 s: two write-backs,
 * then software volume */
static void insist(void)
{
    handover();
    hs.ignores = 1;
    hs.actual = hs.bluez = 127;
    steps(60);
    assert(!dev_ctl && hs.nwrites == 3 && last_mix() == -600);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *run = argv[1];
    hs.transports = 1;
    if (!strcmp(run, "mapping")) {
        for (int p = 0; p <= 100; p++)
            assert(dev_to_pct(pct_to_dev(p)) == p); /* every % reads back as itself */
        for (int d = 0; d <= 127; d++)
            assert(abs(pct_to_dev(dev_to_pct(d)) - d) <= 1);
        assert(pct_to_dev(0) == 0 && pct_to_dev(100) == 127 && pct_to_dev(40) == 51);
        return 0;
    }
    if (!strcmp(run, "off")) { /* the headset there, with a volume: nothing runs */
        hs.actual = hs.bluez = 127;
        boot_bt(0);
        steps(40);
        choose(1, 1);
        step();
        choose(0, 1);
        steps(40);
        assert(spawns == 0 && !hs.nwrites && !dev_ctl && !shanlingq2_device_volume());
        for (int i = 0; i < mixes; i++)
            assert(mix[i].level == -1000 || mix[i].level == saved[mix[i].bt] * 10);
        assert(last_mix() == saved[1] * 10);
    } else if (!strcmp(run, "handover")) {
        /* Q2 Pod left it at 127; Rockbox's headset level is -60 dB, 40 % */
        hs.actual = hs.bluez = 127;
        boot_bt(1);
        assert(hs.nwrites == 1 && hs.writes[0] == 51 && hs.actual == 51);
        steps(4); /* settling: the mixer keeps the volume */
        assert(!dev_ctl && last_mix() == -600);
        steps(4);
        assert(dev_ctl && shanlingq2_device_volume() && last_mix() == 0);
        assert(global_status.volume == -60 && hs.nwrites == 1);
    } else if (!strcmp(run, "late")) {
        /* the hand-over's 127 reported after our write, before and after the take */
        hs.actual = hs.bluez = 127;
        boot_bt(1);
        hs.bluez = 127; /* a report only: it plays at ours */
        steps(8);
        assert(!dev_ctl && hs.nwrites == 2 && hs.writes[1] == 51); /* contradicted: again */
        steps(8);
        assert(dev_ctl && last_mix() == 0);
        hs.bluez = 127;
        steps(8);
        assert(hs.writes[hs.nwrites - 1] == 51 && hs.bluez == 51); /* written over, not followed */
        assert(global_status.volume == -60);
    } else if (!strcmp(run, "car")) {
        /* the car at its own 40, Rockbox at 0 dB: never raised, Rockbox follows */
        saved[1] = 0;
        hs.actual = hs.bluez = 40;
        boot_bt(1);
        steps(10);
        for (int i = 0; i < hs.nwrites; i++)
            assert(hs.writes[i] <= 40);
        assert(dev_ctl && hs.actual == 40 && global_status.volume == dev_to_pct(40) - 100);
    } else if (!strcmp(run, "knob")) {
        /* after the guard: its keys move Rockbox, nothing written back */
        hs.actual = hs.bluez = 127;
        boot_bt(1);
        steps(50);
        int w = hs.nwrites;
        hs.actual = hs.bluez = 20;
        steps(8);
        assert(global_status.volume == dev_to_pct(20) - 100);
        hs.actual = hs.bluez = 90;
        steps(8);
        assert(global_status.volume == dev_to_pct(90) - 100 && hs.nwrites == w);
        sound_set_volume(global_status.volume - 1); /* the wheel, one click down */
        steps(2);
        assert(hs.nwrites == w + 1 && hs.writes[w] == pct_to_dev(dev_to_pct(90) - 1));
        steps(8);
        assert(hs.nwrites == w + 1);
    } else if (!strcmp(run, "limit")) {
        /* Volume Limit -20 dB: its keys can't take it past 80 % */
        limit = -20;
        hs.actual = hs.bluez = 60;
        boot_bt(1);
        steps(50);
        hs.actual = hs.bluez = 127;
        steps(12);
        assert(global_status.volume == -20 && hs.bluez == pct_to_dev(80));
    } else if (!strcmp(run, "ignores")) {
        /* it reports 127 and holds there: the mixer never lets go */
        hs.actual = hs.bluez = 127;
        hs.ignores = 1;
        boot_bt(1);
        steps(200);
        assert(!dev_ctl && hs.nwrites == DEV_TRIES && last_mix() == -600);
    } else if (!strcmp(run, "none")) {
        hs.actual = 100;
        hs.bluez = -1; /* no absolute volume */
        boot_bt(1);
        steps(200);
        assert(!dev_ctl && !hs.nwrites && last_mix() == -600);
    } else if (!strcmp(run, "two")) {
        hs.actual = hs.bluez = 127;
        hs.transports = 2;
        boot_bt(1);
        steps(40);
        assert(!dev_ctl && !hs.nwrites && last_mix() == -600);
    } else if (!strcmp(run, "wired")) {
        /* in control, then wired, then back */
        hs.actual = hs.bluez = 127;
        boot_bt(1);
        steps(10);
        assert(dev_ctl);
        choose(1, 1);
        main_first();
        assert(!bt_output && !dev_ctl && last_mix() == saved[0] * 10);
        steps(10);
        hs.actual = hs.bluez = 100; /* changed meanwhile, by its own keys */
        choose(0, 1);
        step();
        assert(!dev_ctl && last_mix() == saved[1] * 10);
        steps(10);
        assert(dev_ctl && hs.actual == 51);
    } else if (!strcmp(run, "switch")) {
        /* on while playing, then off while playing */
        hs.actual = hs.bluez = 127;
        boot_bt(0);
        steps(4);
        int w = hs.nwrites;
        shanlingq2_set_device_volume(1); /* -60 dB was software: it starts silent */
        steps(10);
        assert(dev_ctl && hs.writes[w] == 0 && hs.actual == 0);
        assert(global_status.volume == -100 && last_mix() == -1000);
        w = hs.nwrites;
        shanlingq2_set_device_volume(0);
        assert(!dev_ctl && last_mix() == -1000); /* at once */
        steps(40);
        assert(hs.nwrites == w && hs.actual == 0);
    } else if (!strcmp(run, "gone")) {
        /* bluetoothd's transport gone in control */
        hs.actual = hs.bluez = 127;
        boot_bt(1);
        steps(10);
        hs.gone = 1;
        steps(12);
        assert(!dev_ctl && last_mix() == -600);
        hs.gone = 0; /* back from its case: the reattach reopens the PCM */
        steps(40);
        assert(!dev_ctl);
        audiohw_preinit();
        steps(10);
        assert(dev_ctl);
        hs.gone = 1;
        steps(12);
        hs.gone = 0; /* and chosen again */
        choose(1, 1);
        step();
        choose(0, 1);
        steps(10);
        assert(dev_ctl);
    } else if (!strcmp(run, "insist")) {
        insist();
    } else if (!strcmp(run, "held")) {
        /* a reopen doesn't take a headset that held to its own again */
        insist();
        int w = hs.nwrites;
        audiohw_preinit();
        steps(20);
        assert(!dev_ctl && hs.nwrites == w);
    } else if (!strcmp(run, "toggle")) {
        /* a pass that outlived the switch posted; the next on doesn't set it */
        handover();
        shanlingq2_set_device_volume(0);
        dev_post(100);
        sound_set_volume(-50);
        shanlingq2_set_device_volume(1);
        main_first();
        assert(global_status.volume == -50);
        shanlingq2_set_device_volume(0); /* off: nothing written */
        int w = hs.nwrites;
        assert(!dev_set("/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF/sep1/fd0", 10) && hs.nwrites == w);
    } else if (!strcmp(run, "step-take")) {
        /* a car with steps of 4: 53 is answered with 56, which is ours */
        hs.step = 4;
        saved[1] = -58;
        hs.actual = hs.bluez = 127;
        boot_bt(1);
        steps(10);
        assert(dev_ctl && hs.nwrites == 1 && hs.actual == 56);
    } else if (!strcmp(run, "step-click")) {
        /* slow clicks down, each answered with the car's own step */
        handover();
        steps(50);
        hs.step = 4;
        int v = global_status.volume;
        for (int i = 0; i < 4; i++) {
            sound_set_volume(global_status.volume - 1);
            steps(8);
        }
        assert(dev_ctl && global_status.volume == v - 4 && hs.actual <= pct_to_dev(v + 100 - 4) + 4);
    } else if (!strcmp(run, "step-limit")) {
        /* Volume Limit -20 dB on a car with steps: written down once, it holds */
        limit = -20;
        handover();
        hs.step = 4;
        steps(50);
        hs.actual = hs.bluez = 127;
        volume_events = 0;
        steps(80);
        assert(volume_events <= 2 && global_status.volume == -20 && hs.actual == 104);
    } else if (!strcmp(run, "takeover")) {
        /* a second headset linked, the PCM reopened on it: software volume */
        handover();
        hs.transports = 2;
        audiohw_preinit();
        steps(2);
        assert(!dev_ctl && last_mix() == -600);
    } else if (!strcmp(run, "rate")) {
        /* a rate change reopens the same transport: nothing written, no dip */
        handover();
        steps(4);
        int w = hs.nwrites, m = mixes;
        audiohw_preinit();
        steps(8);
        assert(dev_ctl && hs.nwrites == w && mixes == m);
    } else if (!strcmp(run, "lost-take")) {
        /* the take's post swallowed: nothing written above the car's 40 */
        saved[1] = 0;
        hs.actual = hs.bluez = 40;
        swallow = 1;
        boot_bt(1);
        steps(20);
        for (int i = 0; i < hs.nwrites; i++)
            assert(hs.writes[i] <= 40);
        assert(global_status.volume == dev_to_pct(40) - 100);
    } else if (!strcmp(run, "lost-knob")) {
        /* a knob's post swallowed: not written back as a wheel move */
        handover();
        steps(50);
        int w = hs.nwrites;
        hs.actual = hs.bluez = 20;
        swallow = 1;
        steps(20);
        assert(hs.nwrites == w && hs.actual == 20 && global_status.volume == dev_to_pct(20) - 100);
        swallow = 100; /* gone inside it: the software volume back without misc.c */
        hs.gone = 1;
        steps(12);
        assert(!dev_ctl && last_mix() == global_status.volume * 10);
    } else if (!strcmp(run, "slow")) {
        /* one Get timed out in control: tried again, kept */
        handover();
        steps(50);
        hs.fail = 1;
        steps(8);
        assert(dev_ctl && last_mix() == 0);
    } else if (!strcmp(run, "paused")) {
        /* paused in control: the headset isn't read; at play, it is followed */
        handover();
        steps(50);
        playing = AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE;
        int s = spawns;
        hs.actual = hs.bluez = 20;
        steps(20);
        assert(spawns == s && dev_ctl && global_status.volume == -60);
        playing = AUDIO_STATUS_PLAY;
        steps(8);
        assert(global_status.volume == dev_to_pct(20) - 100);
    } else if (!strcmp(run, "race")) {
        /* a take that ends after the output was chosen again stays off */
        hs.actual = hs.bluez = 127;
        boot_bt(1);
        vol_pending = true;
        dev_take(true);
        assert(!dev_ctl);
        vol_pending = false;
        dev_retake = true;
        dev_take(true);
        assert(!dev_ctl);
        dev_retake = false;
        dev_link = false;
        dev_take(true);
        assert(!dev_ctl);
        return 0;
    } else
        return 2;
    check();
    return 0;
}
''')
    subprocess.run(["cc", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                    "-pthread", f"-I{tmp}", f"-I{root}", str(tmp / "test.c"),
                    "-o", str(tmp / "test")], check=True)
    for run in ("mapping", "off", "handover", "late", "car", "knob", "limit",
                "ignores", "none", "two", "wired", "switch", "gone", "race",
                "insist", "held", "toggle", "step-take", "step-click", "step-limit",
                "takeover", "rate", "lost-take", "lost-knob", "slow", "paused"):
        subprocess.run([str(tmp / "test"), run], check=True, timeout=30)
print("Q2 Control Device Volume checks passed")
