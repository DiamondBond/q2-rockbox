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
 * it at power on, so a rate change before the wake is fine.
 *
 * Driver calls: only dac_thread calls the driver. Every other thread, SIGIO
 * included, sets what the DAC should be (want_*) and sends a byte down a pipe.
 * Each call waits on the DAC's I2C, and on a Q2 whose bus had stopped answering
 * one waited 23 s; Rockbox's threads all run on the main thread, so the screen
 * and keys went with it. A start still waits for the power on, as it did, but
 * for 1.5 s at most. */
#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "config.h"
#include "audiohw.h"
#include "button.h"
#include "kernel.h"
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
/* What the DAC should be: set from any thread, SIGIO included (plain stores,
 * then a byte down kick_pipe). want_on also changes between the tick and the
 * main thread, under want_mtx, so an idle power off can't undo a wake. */
static volatile int want_on, want_mute = -1, want_gain, want_filter;
static pthread_mutex_t want_mtx = PTHREAD_MUTEX_INITIALIZER;
/* What it is: written only by dac_thread */
static volatile bool dac_on;
static volatile long idle_since; /* tick of the last stop; 0: not idle */
static int kick_pipe[2] = { -1, -1 };
static unsigned asked, applied; /* requests made; the last one dac_thread applied */
static pthread_mutex_t applied_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t applied_cond; /* on CLOCK_MONOTONIC: dac_thread_start */

static void dac_set(unsigned long req, int v)
{
    if (dac >= 0)
        ioctl(dac, req, &v);
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

/* dac_thread only: make the DAC what want_* says. Powering on restores all a
 * powered-off DAC has lost, in stock's order; the driver sleeps about 530 ms in
 * there (100 in HEADSET, 428 in POWER), and its last sleep plus the 10 jiffies
 * it waits to unmute are settling enough, as for stock. */
static void dac_apply(void)
{
    static int cur_mute = -1, cur_gain = -1, cur_filter = -1;
    if (want_on && !dac_on && dac >= 0) {
        dac_set(DAC_HEADSET, 1);
        dac_set(DAC_POWER, 1);
        dac_set(DAC_PCM, 0);
        dac_set(DAC_VOLUME, 100 << 8 | 100);
        cur_mute = cur_gain = cur_filter = -1;
        dac_on = true;
    } else if (!want_on && dac_on) {
        /* Decided under want_mtx, where a wake reads dac_on: the wake either
         * keeps it on or sees it off and waits for the power on. The stored
         * mute and volume stay: the driver's mute holds through the wake,
         * which then restores ours. */
        pthread_mutex_lock(&want_mtx);
        bool off = !want_on;
        if (off)
            dac_on = false;
        pthread_mutex_unlock(&want_mtx);
        if (!off)
            return;
        dac_set(DAC_MUTE, 1);
        dac_set(DAC_POWER, 0);
        return;
    }
    if (!dac_on)
        return;
    if (want_gain != cur_gain)
        dac_set(DAC_GAIN, cur_gain = want_gain);
    if (want_filter != cur_filter)
        dac_set(DAC_FILTER, cur_filter = want_filter);
    int m = want_mute;
    if (m >= 0 && m != cur_mute)
        dac_set(DAC_MUTE, cur_mute = m); /* a later call: the DAC's own state */
}

static void *dac_thread(void *arg)
{
    (void)arg;
    for (;;) {
        char b[16];
        ssize_t n = read(kick_pipe[0], b, sizeof b);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return NULL;
        unsigned seen = __atomic_load_n(&asked, __ATOMIC_SEQ_CST);
        dac_apply();
        pthread_mutex_lock(&applied_mtx);
        applied = seen;
        pthread_cond_broadcast(&applied_cond);
        pthread_mutex_unlock(&applied_mtx);
    }
}

/* Async-signal-safe: an atomic add and a write that never blocks */
static unsigned dac_kick(void)
{
    unsigned n = __atomic_add_fetch(&asked, 1, __ATOMIC_SEQ_CST);
    if (kick_pipe[1] >= 0) {
        char c = 0;
        ssize_t r = write(kick_pipe[1], &c, 1); /* a full pipe: one is pending */
        (void)r;
    }
    return n;
}

/* Until dac_thread has applied request n, or ms have passed */
static void dac_wait(unsigned n, int ms)
{
    struct timespec until;
    if (kick_pipe[0] < 0)
        return; /* no dac_thread: nothing would answer */
    clock_gettime(CLOCK_MONOTONIC, &until);
    until.tv_sec += ms / 1000;
    until.tv_nsec += (ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&applied_mtx);
    while ((int)(applied - n) < 0)
        if (pthread_cond_timedwait(&applied_cond, &applied_mtx, &until) == ETIMEDOUT)
            break;
    pthread_mutex_unlock(&applied_mtx);
}

/* Once, before the first driver call. Signals blocked: Rockbox's own (its tick,
 * SIGIO) stay with the threads they are for. */
static void dac_thread_start(void)
{
    if (kick_pipe[0] >= 0 || pipe2(kick_pipe, O_CLOEXEC) < 0)
        return;
    fcntl(kick_pipe[1], F_SETFL, O_NONBLOCK);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&applied_cond, &ca);
    pthread_condattr_destroy(&ca);
    sigset_t all, old;
    pthread_t t;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    if (pthread_create(&t, NULL, dac_thread, NULL) != 0) {
        close(kick_pipe[0]);
        close(kick_pipe[1]);
        kick_pipe[0] = kick_pipe[1] = -1;
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);
}

/* pcm-alsa calls this before opening the PCM, and again when the headset is
 * lost, to choose the device again */
void audiohw_preinit(void)
{
    bt_output = !headphones_inserted() && bt_connected();
    if (bt_output) {
        pcm_alsa_set_playback_device(BT_DEVICE);
        audiohw_mute(false);
        return;
    }
    pcm_alsa_set_playback_device(DAC_DEVICE);
    /* the driver loads in S11; stock waits up to 2 s for the node */
    for (int i = 0; i < 40 && dac < 0; i++) {
        dac = open("/dev/shanling_dac", O_RDWR | O_CLOEXEC);
        if (dac < 0)
            usleep(50000);
    }
    dac_thread_start();
    pthread_mutex_lock(&want_mtx);
    want_on = 1;
    pthread_mutex_unlock(&want_mtx);
    audiohw_mute(false);
    dac_wait(dac_kick(), 2000);
}

static bool dac_idle_expired(void)
{
    long since = idle_since;
    return want_on && since && !TIME_BEFORE(current_tick, since + DAC_IDLE_TICKS);
}

/* On the tick thread: asks dac_thread for the power off (about 300 ms in the
 * driver) and carries on */
static void dac_idle_tick(void)
{
    if (!dac_idle_expired())
        return;
    pthread_mutex_lock(&want_mtx);
    bool off = dac_idle_expired(); /* a wake may have just cleared it */
    if (off)
        want_on = 0;
    pthread_mutex_unlock(&want_mtx);
    if (off)
        dac_kick();
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
    pthread_mutex_lock(&want_mtx);
    idle_since = 0;
    if (!bt_output)
        want_on = 1;
    bool on = dac_on;
    pthread_mutex_unlock(&want_mtx);
    if (bt_output)
        return;
    unsigned n = dac_kick();
    if (!on)
        dac_wait(n, 1500); /* the power on, as before, after an idle power off
                            * it may be finishing; never longer than 1.5 s */
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

void audiohw_close(void)
{
    audiohw_mute(true);
    pthread_mutex_lock(&want_mtx);
    want_on = 0;
    pthread_mutex_unlock(&want_mtx);
    unsigned n = dac_kick();
    bool thread = kick_pipe[0] >= 0; /* none while Bluetooth was the only output */
    if (thread)
        dac_wait(n, 1000);
    /* dac_thread may still be in the driver if it didn't answer: leave the fd
     * open for it then (Rockbox is exiting) */
    pthread_mutex_lock(&applied_mtx);
    bool idle = !thread || (int)(applied - n) >= 0;
    pthread_mutex_unlock(&applied_mtx);
    if (idle && dac >= 0) {
        close(dac);
        dac = -1;
    }
}

void audiohw_set_frequency(int fsel)
{
    (void)fsel;
}

void audiohw_set_volume(int vol_l, int vol_r)
{
    vol[0] = vol_l;
    vol[1] = vol_r;
    if (!muted)
        pcm_set_mixer_volume(vol_l, vol_r);
}

void audiohw_mute(int mute)
{
    if (muted == mute)
        return;
    muted = mute;
    /* sink_dma_stop can call this in SIGIO: no lock, no driver call here */
    want_mute = mute ? 1 : 0;
    dac_kick();
    if (mute)
        pcm_set_mixer_volume(-1000, -1000); /* tenths of a dB */
    else
        pcm_set_mixer_volume(vol[0], vol[1]);
}

void audiohw_set_power_mode(int mode)
{
    gain = mode == SOUND_HIGH_POWER;
    want_gain = gain;
    dac_kick();
}

void audiohw_set_filter_roll_off(int value)
{
    if ((unsigned)value > 3)
        return;
    filter = value;
    want_filter = filter;
    dac_kick();
}
