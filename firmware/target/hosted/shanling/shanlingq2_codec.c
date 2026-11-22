/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 Diamond Bond
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

/* The Q2's CS43131 DAC, ALSA card 0, is driven through /dev/shanling_dac as
 * stock demo and hciplayer do (tools/shanlingq2/README). The DAC sits at its
 * 0 dB step and Rockbox's volume is all in pcm-alsa's 32-bit mixer.
 * ponytail: the DAC's own steps (0.5 dB from about -38 dB up, coarser below,
 * per the driver's tables) could take the coarse part, if the noise floor at
 * low volume ever matters.
 *
 * Bluetooth: Q2 Pod starts Rockbox from its Home menu with the headset it
 * connected still connected (its daemons outlive the stock player), so a
 * headset that bluez-alsa's "bluealsa" PCM opens on is the output, and the DAC
 * stays off. "bluealsa" is the most recently connected device through plug,
 * which converts Rockbox's rate and 32-bit samples to the link's. pcm-alsa
 * feeds it from a thread (it has no async callback), and falls back here,
 * to the DAC, when the headset goes away. Headphones in either jack win over
 * Bluetooth: bluez-alsa can still hold a PCM for a headset that isn't in
 * anyone's ears (earbuds linked from their case), and that played to nobody.
 *
 * Idle: as stock's check_dacoff_state, the DAC powers off DAC_IDLE_TICKS after
 * pcm-alsa stops and on again before it starts, the PCM left open throughout.
 * The driver keeps the rate from hw_params, without I2C while off, and applies
 * it at power on, so a rate change before the wake is fine. */
#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "config.h"
#include "audio.h"
#include "audiohw.h"
#include "button.h"
#include "kernel.h"
#include "panic.h"
#include "pcm-alsa.h"

#define DAC_VOLUME   0xc0044d00 /* left << 8 | right, each 0..100 (0 dB) */
#define DAC_POWER    0xc0044d1a /* 1 on, 0 off */
#define DAC_HEADSET  0xc0044d1c /* 1: output to the DAC's jacks */
#define DAC_PCM      0xc0044d1b /* 0: PCM, not DSD */
#define DAC_MUTE     0xc0044d1f
#define DAC_GAIN     0xc0044d0d /* 1 high, 0 low (mclSetGainMode) */
#define DAC_FILTER   0xc0044d19 /* fast, slow, low-delay fast, low-delay slow */

#define DAC_DEVICE "plughw:0,0" /* pcm-alsa's default */
#define BT_DEVICE  "bluealsa"   /* bluez-alsa's 20-bluealsa.conf */

#define DAC_IDLE_TICKS (30 * HZ)

static int dac = -1;
static int gain = 0;
static int filter = 0;
static int muted = -1;
static int vol[2] = { -1000, -1000 }; /* silent until Rockbox sets its volume */
static bool bt_output;

/* An event for Rockbox's main thread (misc.c): the output chosen, the latest
 * value. Rockbox's queues aren't for its other threads, so the tick thread's
 * button_read_device posts it, as it posts the keys. Only the newest matters,
 * so one slot, and a slow tick can't lose the newest. */
static volatile int ev_output = -1; /* 0 DAC, 1 Bluetooth; -1 none pending */
/* The output changed: silent until Rockbox sets the volume that goes with it
 * (misc.c's setvol), not the other output's level meanwhile */
static volatile bool vol_pending;
/* Control Device Volume (below): the setting; the headset carries the volume
 * and the mixer only the balance; and the headset's volume for misc.c, in dB
 * (1: none pending) */
static volatile bool dev_link;
static volatile bool dev_ctl;
static volatile int ev_volume = 1;
static volatile int vol_wait = 1; /* the number posted to misc.c, until it has set it; 1: none */
static volatile bool dev_retake; /* a headset chosen, or the setting on: take it from the start */
static volatile bool dev_recheck; /* the PCM reopened on Bluetooth: a rate change, or a headset back */
static volatile bool dev_zero; /* switched on while Rockbox runs: its Bluetooth level was a dB, not a position */

/* The mixer's level when it isn't silent: the volume, or under Control Device
 * Volume only the balance (the minimum stays the mixer's, as a headset at 0
 * may still be heard) */
static void mixer_level(void)
{
    int l = vol[0], r = vol[1], m = MAX(l, r); /* once: main may set them meanwhile */
    if (!__atomic_load_n(&dev_ctl, __ATOMIC_SEQ_CST) || !dev_link || m <= -1000) {
        pcm_set_mixer_volume(l, r);
        return;
    }
    pcm_set_mixer_volume(l - m, r - m);
    /* audiohw_preinit may have chosen again meanwhile (it sets vol_pending,
     * then silences): never the headset's balance on what follows */
    if (__atomic_load_n(&vol_pending, __ATOMIC_SEQ_CST))
        pcm_set_mixer_volume(-1000, -1000);
}

/* Only for the output in use: an ack for the one before leaves the silence to
 * the event on its way */
void shanlingq2_volume_ack(bool bt)
{
    if (bt != bt_output)
        return;
    vol_pending = false;
    if (!muted)
        mixer_level();
}

/* The newest number, or else the one misc.c hasn't set yet, again, as the
 * output's: a plugin may have swallowed it */
static bool take_volume(long *id, intptr_t *data)
{
    static long again;
    int v = __atomic_exchange_n(&ev_volume, 1, __ATOMIC_ACQ_REL);
    if (v > 0) {
        v = __atomic_load_n(&vol_wait, __ATOMIC_SEQ_CST);
        if (v > 0 || !button_queue_empty() || TIME_BEFORE(current_tick, again))
            return false;
    }
    again = current_tick + HZ;
    *id = SYS_VOLUME_CHANGED;
    *data = v;
    return true;
}

bool shanlingq2_take_event(long *id, intptr_t *data)
{
    static long again;
    int o = __atomic_exchange_n(&ev_output, -1, __ATOMIC_ACQ_REL);
    if (o < 0 && vol_pending && button_queue_empty() && TIME_AFTER(current_tick, again))
        o = bt_output; /* a screen dropped it (cleared the queue): again */
    if (o < 0)
        return dev_link && take_volume(id, data);
    again = current_tick + HZ;
    *id = SYS_OUTPUT_CHANGED;
    *data = o;
    return true;
}
/* dac_on, across the tick, main and writer threads. Never from SIGIO, which
 * is why audiohw_mute doesn't take it. Never held across an ioctl: those are
 * made by the thread below. */
static pthread_mutex_t dac_mtx = PTHREAD_MUTEX_INITIALIZER;
static volatile bool dac_on;
static volatile long idle_since; /* tick of the last stop; 0: not idle */

/* The DAC's ioctls, from one thread of their own. A DAC_MUTE ioctl was seen
 * to stall 23 s in the driver's I2C, and whoever made it stalled with it: the
 * tick thread (the clock), the writer (audio) or main (the UI). (A second 23 s
 * wait seen then was in ALSA's hw_params, which this doesn't cover.) A caller
 * writes {request, value} to a pipe instead, one write under PIPE_BUF, never
 * torn, and the thread makes the ioctls in that order. One with no wait behind
 * it (a stop's or a start's mute, a gain or filter change) can be in the driver
 * while ALSA calls are; one a wait below times out on can land after an ALSA
 * call it used to precede. Where the driver has to have caught up (the
 * power-on before the PCM opens or fills, the stop's mute before a rate
 * change, the power-off before the node is closed), the caller waits for a
 * marker, for a bounded time: shanlingq2_dac_sync. */
struct dac_req {
    unsigned long req;
    int value;
};
#define DAC_SYNC 0 /* a marker: value is its number; no ioctl */
static int dac_pipe[2] = { -1, -1 };
static pthread_mutex_t sync_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sync_cond;
static unsigned sync_sent, sync_done; /* marker numbers: the last queued, the last reached */

/* Also from SIGIO (audiohw_mute): a write that never blocks. A full pipe (64 KB
 * of requests behind a stalled driver) drops the request. */
static void dac_set(unsigned long req, int v)
{
    struct dac_req r = { req, v };
    ssize_t n = write(dac_pipe[1], &r, sizeof r);
    (void)n;
}

static void *dac_thread(void *arg)
{
    (void)arg;
    struct dac_req r;
    /* whole records only (each write is one); signals are blocked here */
    while (read(dac_pipe[0], &r, sizeof r) == (ssize_t)sizeof r) {
        if (r.req == DAC_SYNC) {
            pthread_mutex_lock(&sync_mtx);
            sync_done = (unsigned)r.value;
            pthread_cond_broadcast(&sync_cond);
            pthread_mutex_unlock(&sync_mtx);
            continue;
        }
        int fd = dac; /* -1: closed or never opened; dropped */
        if (fd >= 0)
            ioctl(fd, r.req, &r.value);
    }
    return NULL;
}

/* Waits until the thread has made every ioctl queued so far, at most ms;
 * true if it did. Not from SIGIO. */
bool shanlingq2_dac_sync(int ms)
{
    /* numbered and written under the lock: markers reach the pipe in number
     * order, so sync_done only grows (the write never blocks) */
    pthread_mutex_lock(&sync_mtx);
    unsigned mine = ++sync_sent;
    struct dac_req r = { DAC_SYNC, (int)mine };
    bool queued = write(dac_pipe[1], &r, sizeof r) == (ssize_t)sizeof r;
    pthread_mutex_unlock(&sync_mtx);
    if (!queued)
        return false;
    struct timespec until;
    clock_gettime(CLOCK_MONOTONIC, &until);
    until.tv_sec += ms / 1000;
    until.tv_nsec += (ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_nsec -= 1000000000L;
        until.tv_sec++;
    }
    pthread_mutex_lock(&sync_mtx);
    while ((int)(sync_done - mine) < 0 &&
           pthread_cond_timedwait(&sync_cond, &sync_mtx, &until) == 0)
        ;
    bool reached = (int)(sync_done - mine) >= 0;
    pthread_mutex_unlock(&sync_mtx);
    return reached;
}

/* Once, at the first preinit (sink_dma_init, on main), whichever output
 * comes first: signals blocked as for Rockbox's other threads of their own.
 * Without the thread nothing would reach the DAC, so a failure is a panic, as
 * for the PCM writer. */
static void dac_thread_start(void)
{
    if (dac_pipe[0] >= 0)
        return;
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&sync_cond, &ca);
    pthread_condattr_destroy(&ca);
    /* the reader blocks; the writers (SIGIO among them) never do */
    if (pipe2(dac_pipe, O_CLOEXEC) < 0 || fcntl(dac_pipe[1], F_SETFL, O_NONBLOCK) < 0)
        panicf("Unable to open the DAC thread's pipe: %s", strerror(errno));
    sigset_t all, old;
    pthread_t t;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    int err = pthread_create(&t, NULL, dac_thread, NULL);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (err)
        panicf("Unable to start the DAC thread: %s", strerror(err));
}

/* A headset bluez-alsa has a PCM for; no daemon (a cold boot) fails at once */
static bool bt_connected(void)
{
    snd_pcm_t *pcm;
    if (snd_pcm_open(&pcm, BT_DEVICE, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) < 0)
        return false;
    snd_pcm_close(pcm);
    return true;
}

/* The headset's play state. bluetoothd answers the headset's "playing?" with
 * what Shanling's shanling.bluez NotifyChanged last set: event 0x107, one AVRCP
 * status byte (libbtctl's btctl_notify_playback_status_changed), which stock's
 * player sends and Rockbox didn't. AirPods send next and previous only to a
 * source that says it plays, so every press was Play. A thread of its own
 * sends each change of audio_status() through dbus-send, so no Rockbox thread
 * waits on the bus. */
#define BT_STOPPED 0
#define BT_PLAYING 1
#define BT_PAUSED  2

static volatile bool bt_again; /* a headset chosen (again): send even no change */

static void bt_status_send(int status)
{
    extern char **environ;
    char data[16];
    snprintf(data, sizeof data, "array:byte:%d", status);
    /* --print-reply: wait for bluetoothd's answer, at most the reply timeout,
     * so one send ends before the next; the reply goes nowhere */
    char *argv[] = { "dbus-send", "--system", "--print-reply", "--reply-timeout=2000",
                     "--dest=shanling.bluez", "/shanling/bluez",
                     "shanling.bluez.NotifyChanged", "int32:263", data, NULL };
    posix_spawnattr_t attr;
    posix_spawn_file_actions_t fa;
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_init(&attr);
    posix_spawnattr_setsigmask(&attr, &none); /* this thread blocks them all */
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK);
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    pid_t pid;
    int err = posix_spawn(&pid, "/usr/bin/dbus-send", &fa, &attr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (!err)
        waitpid(pid, NULL, 0);
}

/* One look; *sent is what the headset was last told, -1 nothing. One try per
 * change: a failure waits for the next pause or play, not a retry storm. */
static void bt_status_check(int *sent)
{
    if (!bt_output) /* a return to Bluetooth sets bt_again */
        return;
    if (bt_again) {
        bt_again = false;
        *sent = -1;
    }
    int a = audio_status();
    int status = !(a & AUDIO_STATUS_PLAY) ? BT_STOPPED :
                 (a & AUDIO_STATUS_PAUSE) ? BT_PAUSED : BT_PLAYING;
    if (status != *sent) {
        bt_status_send(status);
        *sent = status;
    }
}

/* Control Device Volume (Sound Settings), as Q2 Pod's: off, all of this stays
 * still. On, while a headset whose absolute volume BlueZ knows is the output,
 * Rockbox's volume is that volume: a position on its 0..127, written and read
 * as BlueZ's MediaTransport1 Volume (the property Q2 Pod's libbtctl sets),
 * through dbus-send on this thread, so no Rockbox thread waits on the bus.
 * Rockbox's -100..0 dB are read as 0..100 % of it, as Q2 Pod reads its volume,
 * and the mixer keeps only the balance; the headset's keys or a car's knob move
 * Rockbox's number (SYS_VOLUME_CHANGED to misc.c).
 *
 * bluealsa isn't the way: Q2 Pod and stock run it without --a2dp-volume, so its
 * ctl's "A2DP Playback Volume" is bluealsa's own software gain and never
 * reaches the headset, and the headset's reports skip it (bluez-alsa 4.0.0).
 * BlueZ 5.66 has the Volume of a headset the Q2 plays to only once the headset
 * has reported it, and changes it only on the headset's answer to a write or
 * its own report, never on the write itself; so a read after a write is the
 * headset's word, and a device with no absolute volume has none to read.
 *
 * Never louder by surprise. The mixer keeps the volume until a read-back
 * confirms the headset at Rockbox's level, and gets it back at every doubt. A
 * take never raises the headset: it is written to Rockbox's level or left
 * lower, and then Rockbox's number follows it; switched on while Rockbox
 * runs, it is written 0, as Rockbox's level was a dB. A raise the headset
 * reports in the first DEV_GUARD after a take is written over, twice at most,
 * not followed: Q2 Pod's hand-over raises it to its maximum, and that report can
 * come after ours. Rockbox's number is written as the wheel's only once misc.c
 * has set the one posted (shanlingq2_volume_set), so a lost post is never
 * taken for the wheel. */
#define DEV_SETTLE (3 * HZ / 2) /* a write's answer, and the hand-over's late one */
#define DEV_GUARD  (10 * HZ)
#define DEV_TRIES  10           /* takes a headset gets, about one a second */
#define DEV_STEP   4 /* a car answers a write with its own level, this far off (Tesla: 55 -> 58) */
/* ponytail: a dbus-send a second in control, three a take: a libdbus
 * connection of this thread's own if the battery ever shows it */

/* The headset's 0..127 and Rockbox's 0..100 %, nearest both ways: every % reads
 * back as itself */
static int pct_to_dev(int pct)
{
    return (pct * 127 + 50) / 100;
}

static int dev_to_pct(int d)
{
    return (d * 100 + 63) / 127;
}

/* Rockbox's volume for this output, as % (vol[] is in tenths of a dB) */
static int vol_pct(void)
{
    int m = MAX(vol[0], vol[1]);
    return m < -1000 ? 0 : m > 0 ? 100 : (m + 1000) / 10;
}

/* argv's program, its output in out (NUL-terminated); true if it exits 0 */
static bool dev_run(char *const argv[], char *out, size_t size)
{
    extern char **environ;
    int fd[2];
    if (pipe2(fd, O_CLOEXEC) < 0)
        return false;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_t fa;
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_init(&attr);
    posix_spawnattr_setsigmask(&attr, &none); /* this thread blocks them all */
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK);
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    int err = posix_spawn(&pid, argv[0], &fa, &attr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(fd[1]);
    size_t n = 0;
    ssize_t r;
    /* past size, the program's next write fails and so does the run */
    while (!err && n + 1 < size && (r = read(fd[0], out + n, size - 1 - n)) > 0)
        n += r;
    out[n] = '\0';
    close(fd[0]);
    int status;
    return !err && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
           !WEXITSTATUS(status);
}

#define DEV_SEND "/usr/bin/dbus-send", "--system", "--print-reply", \
                 "--reply-timeout=2000", "--dest=org.bluez"

/* The one A2DP transport BlueZ has (its path ends in /fdN); none or two: false */
static bool dev_find(char *path, size_t size)
{
    char *argv[] = { "/bin/sh", "-c", "/usr/bin/dbus-send --system --print-reply "
        "--reply-timeout=2000 --dest=org.bluez / "
        "org.freedesktop.DBus.ObjectManager.GetManagedObjects | /bin/sed -n "
        "'s|^ *object path \"\\(/org/bluez/[^\"]*/fd[0-9]*\\)\"$|\\1|p'", NULL };
    char out[256];
    if (!dev_run(argv, out, sizeof out))
        return false;
    char *nl = strchr(out, '\n');
    if (!nl || nl[1] || nl == out || (size_t)(nl - out) >= size)
        return false;
    *nl = '\0';
    strcpy(path, out);
    return true;
}

/* The headset's volume, 0..127, or -1: no transport, or no absolute volume */
static int dev_get(const char *path)
{
    char *argv[] = { DEV_SEND, (char *)path, "org.freedesktop.DBus.Properties.Get",
                     "string:org.bluez.MediaTransport1", "string:Volume", NULL };
    char out[512];
    if (!dev_run(argv, out, sizeof out))
        return -1;
    char *u = strstr(out, "uint16 ");
    if (!u)
        return -1;
    char *end;
    long v = strtol(u + 7, &end, 10);
    return end > u + 7 && v >= 0 && v <= 127 ? (int)v : -1;
}

static bool dev_set(const char *path, int v)
{
    if (!__atomic_load_n(&dev_link, __ATOMIC_SEQ_CST))
        return false; /* a pass that outlived the setting: Off writes nothing */
    char arg[24];
    snprintf(arg, sizeof arg, "variant:uint16:%d", v);
    char *argv[] = { DEV_SEND, (char *)path, "org.freedesktop.DBus.Properties.Set",
                     "string:org.bluez.MediaTransport1", "string:Volume", arg, NULL };
    char out[256];
    return dev_run(argv, out, sizeof out);
}

/* For misc.c: Rockbox's number (whole dB), set again so the mixer takes dev_ctl */
static void dev_post(int pct)
{
    __atomic_store_n(&vol_wait, pct - 100, __ATOMIC_SEQ_CST); /* first: a re-post in between sends this one */
    __atomic_store_n(&ev_volume, pct - 100, __ATOMIC_RELEASE);
}

/* misc.c, once it has set (or, for another output, dropped) the number: only
 * the one pending counts, not an older one still queued */
void shanlingq2_volume_set(int volume)
{
    __atomic_compare_exchange_n(&vol_wait, &volume, 1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

/* dev_ctl on, unless the setting went off or the output was chosen again
 * meanwhile (a D-Bus call can take 2 s): those clear dev_ctl after their own
 * flags, so one of the two sees the other */
static void dev_take(bool on)
{
    __atomic_store_n(&dev_ctl, on, __ATOMIC_SEQ_CST);
    if (on && (!__atomic_load_n(&dev_link, __ATOMIC_SEQ_CST) ||
               __atomic_load_n(&vol_pending, __ATOMIC_SEQ_CST) ||
               __atomic_load_n(&dev_retake, __ATOMIC_SEQ_CST) ||
               !__atomic_load_n(&bt_output, __ATOMIC_SEQ_CST)))
        __atomic_store_n(&dev_ctl, false, __ATOMIC_SEQ_CST);
}

enum { DEV_TAKE, DEV_WROTE, DEV_CTL };

/* One look, from bt_status_thread, a quarter second apart.
 * ponytail: with two headsets linked, a reopen on the other one plays it at
 * its own level with the mixer at 0 dB until the recheck below has looked
 * (about a second); preinit putting the software volume back at every reopen
 * would close it, at a dip on every rate change. */
static void dev_volume_pass(void)
{
    static char path[128];
    static int state, tries, fails;
    static int last;      /* the headset's, as written or reported */
    static int before;    /* DEV_TAKE: the headset's before our write */
    static int seen;      /* Rockbox's % last acted on */
    static bool ours;     /* written since the last read */
    static bool lost;     /* dropped on a failed read or write */
    static long took_at, next;
    if (!dev_link)
        return;
    if (__atomic_exchange_n(&dev_recheck, false, __ATOMIC_ACQ_REL)) {
        char p[sizeof path];
        if (lost || state == DEV_WROTE ||
            (state == DEV_CTL && (!dev_find(p, sizeof p) || strcmp(p, path))))
            dev_retake = true; /* a headset back, or the PCM on another transport */
    }
    if (__atomic_exchange_n(&dev_retake, false, __ATOMIC_ACQ_REL)) {
        if (dev_ctl) {
            dev_take(false);
            if (!muted && !vol_pending)
                mixer_level();
            dev_post(vol_pct());
        }
        state = DEV_TAKE;
        tries = 0;
        lost = false;
        next = current_tick;
    }
    if (vol_pending || !bt_output)
        return; /* once these clear, vol[] is this output's */
    bool set = __atomic_load_n(&vol_wait, __ATOMIC_SEQ_CST) > 0; /* and holds what misc.c set */
    int pct = vol_pct();
    long now = current_tick;
    int r;
    switch (state) {
    case DEV_TAKE:
        if (tries >= DEV_TRIES || TIME_BEFORE(now, next))
            return;
        tries++;
        next = now + HZ;
        if (!dev_find(path, sizeof path) || (r = dev_get(path)) < 0)
            return; /* no absolute volume (yet): software volume */
        before = r;
        seen = pct; /* the number this take is for */
        /* never above where it is; after a switch, silent */
        last = dev_zero ? 0 : MIN(pct_to_dev(pct), r);
        if (!dev_set(path, last))
            return;
        next = now + DEV_SETTLE;
        state = DEV_WROTE;
        return;
    case DEV_WROTE:
        if (TIME_BEFORE(now, next))
            return;
        r = dev_get(path);
        if (r < 0 || r > before || abs(r - last) > DEV_STEP || vol_pct() != seen) {
            state = DEV_TAKE; /* contradicted, or the wheel moved: again, from what it says now */
            return;
        }
        last = r;
        seen = dev_to_pct(r);
        tries = fails = 0;
        ours = false;
        took_at = now;
        next = now + HZ;
        state = DEV_CTL;
        dev_take(true);
        if (!__atomic_load_n(&dev_ctl, __ATOMIC_SEQ_CST))
            return; /* switched, or chosen again, meanwhile: the retake starts over */
        dev_zero = false;
        dev_post(seen); /* the mixer leaves the volume to it */
        return;
    }
    /* DEV_CTL: Rockbox's number moved (the wheel, a limit, a screen) */
    if (set && pct != seen) {
        seen = pct;
        if (pct != dev_to_pct(last)) {
            if (!dev_set(path, pct_to_dev(pct)))
                goto drop;
            last = pct_to_dev(pct);
            ours = true;
            next = now + DEV_SETTLE;
        }
        return;
    }
    if (TIME_BEFORE(now, next))
        return;
    /* paused, its keys show up at play: only our write's answer and the
     * guard's window are read meanwhile */
    if (!ours && TIME_AFTER(now, took_at + DEV_GUARD) &&
        (audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE)) != AUDIO_STATUS_PLAY)
        return;
    next = now + HZ;
    r = dev_get(path);
    if (r < 0) {
        if (++fails < 2)
            return; /* a slow bluetoothd: once more, a second later */
        goto drop; /* the transport or its volume gone */
    }
    fails = 0;
    bool answer = ours;
    ours = false;
    if (r == last || (answer && abs(r - last) <= DEV_STEP)) {
        last = r; /* unchanged, or its own step for ours: Rockbox keeps its number */
        return;
    }
    if (r > last && TIME_BEFORE(now, took_at + DEV_GUARD)) {
        if (++tries > 2)
            goto hold; /* it holds to its own: software volume */
        if (!dev_set(path, last))
            goto drop;
        ours = true;
        next = now + DEV_SETTLE;
        return;
    }
    last = r; /* its keys or a car's knob: Rockbox follows, no write back */
    seen = dev_to_pct(r);
    dev_post(seen);
    return;
drop:
    lost = true; /* a read or write failed: taken again when the PCM reopens on Bluetooth */
hold:
    state = DEV_TAKE;
    tries = DEV_TRIES; /* until chosen again, the setting set, or (lost) a reopen */
    dev_take(false);
    if (!muted && !vol_pending)
        mixer_level();
    dev_post(pct);
}

/* Sound Settings' Control Device Volume, and its value at boot */
void shanlingq2_set_device_volume(bool on)
{
    static bool booted; /* the first call is the saved setting at boot */
    bool running = booted;
    booted = true;
    if (on == dev_link)
        return;
    if (on) {
        /* not a number a pass posted before the switch */
        __atomic_store_n(&ev_volume, 1, __ATOMIC_SEQ_CST);
        __atomic_store_n(&vol_wait, 1, __ATOMIC_SEQ_CST);
        /* ponytail: switched on, then a reboot before a headset with absolute
         * volume is taken, loses this: the take then reads the saved dB as a
         * position */
        dev_zero = running;
        dev_retake = true;
        __atomic_store_n(&dev_link, true, __ATOMIC_SEQ_CST);
        return;
    }
    __atomic_store_n(&dev_link, false, __ATOMIC_SEQ_CST);
    __atomic_store_n(&dev_ctl, false, __ATOMIC_SEQ_CST);
    if (!muted && !vol_pending)
        mixer_level(); /* the mixer's volume again, the headset left where it is */
}

/* The headset carries the volume: the skin shows its position */
bool shanlingq2_device_volume(void)
{
    return dev_ctl;
}

/* ponytail: wakes four times a second for as long as Rockbox runs once
 * Bluetooth has been the output, and reports a change up to 250 ms late; wake
 * it from the play-state changes instead if either ever matters. */
static void *bt_status_thread(void *arg)
{
    (void)arg;
    pthread_detach(pthread_self());
    int sent = -1;
    for (;;) {
        usleep(250000);
        bt_status_check(&sent);
        dev_volume_pass();
    }
    return NULL;
}

/* With each Bluetooth output; the thread once, signals blocked as for
 * Rockbox's other threads of their own */
static void bt_status_start(void)
{
    static bool started;
    bt_again = true;
    if (started)
        return;
    sigset_t all, old;
    pthread_t t;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    started = pthread_create(&t, NULL, bt_status_thread, NULL) == 0;
    pthread_sigmask(SIG_SETMASK, &old, NULL);
}

/* All a DAC powered off has lost, queued with dac_mtx held. The driver
 * sleeps about 530 ms making these (100 in HEADSET, 428 in POWER), which the
 * caller's sync waits for; its last sleep plus the 10 jiffies it waits to
 * unmute are settling enough, as for stock. */
static void dac_power_on(void)
{
    dac_set(DAC_HEADSET, 1);
    dac_set(DAC_POWER, 1);
    dac_set(DAC_PCM, 0);
    dac_set(DAC_VOLUME, 100 << 8 | 100);
    dac_set(DAC_GAIN, gain);
    dac_set(DAC_FILTER, filter);
    if (muted >= 0)
        dac_set(DAC_MUTE, muted); /* a later call: the DAC's own state */
    dac_on = dac >= 0;
}

/* pcm-alsa calls this before opening the PCM, and again when the headset is
 * lost, to choose the device again */
void audiohw_preinit(void)
{
    dac_thread_start();
    bool bt = !headphones_inserted() && bt_connected();
    static int was_bt = -1; /* the kind chosen last time; the first counts as a change */
    bool changed = (int)bt != was_bt;
    if (changed) {
        was_bt = bt;
        vol_pending = true; /* before bt_output: the other output's level stays off this one */
        if (dev_link) { /* the new output's take starts over; dev_ctl last (dev_take) */
            ev_volume = 1;
            vol_wait = 1;
            __atomic_store_n(&dev_retake, true, __ATOMIC_SEQ_CST);
            __atomic_store_n(&dev_ctl, false, __ATOMIC_SEQ_CST);
        }
        pcm_set_mixer_volume(-1000, -1000);
    } else if (bt && dev_link)
        dev_recheck = true;
    bt_output = bt;
    if (changed) {
        /* misc.c swaps in this output's volume and sets it, which ends the silence.
         * Not on every preinit: a bluealsa rate change reopens too, and each set
         * is a volume popup and a status write. Last: the set relies on bt_output */
        __atomic_store_n(&ev_output, bt, __ATOMIC_RELEASE);
    }
    if (bt_output) {
        pcm_alsa_set_playback_device(BT_DEVICE);
        /* Taken over from the DAC (unplugged, or the headset back): off now, as
         * on idle, since playback carries on. A fallback powers it on again. */
        pthread_mutex_lock(&dac_mtx);
        if (dac_on) {
            dac_on = false;
            dac_set(DAC_MUTE, 1);
            dac_set(DAC_POWER, 0);
        }
        pthread_mutex_unlock(&dac_mtx);
        audiohw_mute(false);
        bt_status_start();
        return;
    }
    pcm_alsa_set_playback_device(DAC_DEVICE);
    /* the driver loads in S11; stock waits up to 2 s for the node */
    for (int i = 0; i < 40 && dac < 0; i++) {
        dac = open("/dev/shanling_dac", O_RDWR | O_CLOEXEC);
        if (dac < 0)
            usleep(50000);
    }
    pthread_mutex_lock(&dac_mtx);
    dac_power_on();
    audiohw_mute(false);
    pthread_mutex_unlock(&dac_mtx);
    shanlingq2_dac_sync(2000); /* powered before the PCM opens; the driver's 530 ms */
}

static bool dac_idle_expired(void)
{
    long since = idle_since;
    return dac_on && since && !TIME_BEFORE(current_tick, since + DAC_IDLE_TICKS);
}

/* Queued from the tick thread when it expires; the driver takes 300 ms over
 * it on the DAC thread. The stored mute and volume stay as they are: the
 * driver's mute holds through the wake, which then restores ours. */
static void dac_idle_tick(void)
{
    if (!dac_idle_expired())
        return;
    pthread_mutex_lock(&dac_mtx);
    if (dac_idle_expired()) { /* a wake may have just cleared it */
        dac_on = false;
        dac_set(DAC_MUTE, 1);
        dac_set(DAC_POWER, 0);
    }
    pthread_mutex_unlock(&dac_mtx);
}

/* From sink_dma_stop, maybe in SIGIO: no lock, no ioctl */
void shanlingq2_dac_idle(void)
{
    long now = current_tick;
    idle_since = now ? now : 1;
}

/* From sink_dma_start, before any fill */
void shanlingq2_dac_wake(void)
{
    idle_since = 0;
    pthread_mutex_lock(&dac_mtx);
    bool powering = !dac_on && !bt_output;
    if (powering)
        dac_power_on();
    pthread_mutex_unlock(&dac_mtx);
    /* powered before the first fill (behind an idle power-off, about 830 ms);
     * a DAC already on, or Bluetooth, doesn't wait */
    if (powering)
        shanlingq2_dac_sync(2000);
}

/* Not in preinit: writer_reopen calls that during playback */
void audiohw_postinit(void)
{
    shanlingq2_dac_idle();
    tick_add_task(dac_idle_tick);
}

bool shanlingq2_bt_output(void)
{
    return bt_output;
}

/* At shutdown (powermgmt). Behind a stall longer than the wait, the node and
 * the queue are left as they are: the power-off is made only if the stall ends
 * before the process exits, and the exit drops what is still queued and closes
 * the node. */
void audiohw_close(void)
{
    pthread_mutex_lock(&dac_mtx);
    audiohw_mute(true);
    dac_set(DAC_POWER, 0);
    dac_on = false;
    pthread_mutex_unlock(&dac_mtx);
    if (!shanlingq2_dac_sync(1000)) /* the power-off made before the node goes */
        return;
    int fd = dac;
    dac = -1;
    if (fd >= 0)
        close(fd);
}

void audiohw_set_frequency(int fsel)
{
    (void)fsel;
}

void audiohw_set_volume(int vol_l, int vol_r)
{
    vol[0] = vol_l;
    vol[1] = vol_r;
    if (!muted && !vol_pending)
        mixer_level();
}

void audiohw_mute(int mute)
{
    if (muted == mute)
        return;
    muted = mute;
    /* No dac_mtx: sink_dma_stop can call this in SIGIO. Nor is it needed there:
     * the wake that started playback cleared the stamp, so nothing powers off
     * until the stop that follows this. */
    if (dac_on)
        dac_set(DAC_MUTE, mute ? 1 : 0);
    if (mute || vol_pending)
        pcm_set_mixer_volume(-1000, -1000); /* tenths of a dB */
    else
        mixer_level();
}

void audiohw_set_power_mode(int mode)
{
    gain = mode == SOUND_HIGH_POWER;
    pthread_mutex_lock(&dac_mtx);
    if (dac_on)
        dac_set(DAC_GAIN, gain);
    pthread_mutex_unlock(&dac_mtx);
}

void audiohw_set_filter_roll_off(int value)
{
    if ((unsigned)value > 3)
        return;
    filter = value;
    pthread_mutex_lock(&dac_mtx);
    if (dac_on)
        dac_set(DAC_FILTER, filter);
    pthread_mutex_unlock(&dac_mtx);
}
