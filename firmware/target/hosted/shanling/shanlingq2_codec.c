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
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
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

/* An event for Rockbox's main thread (misc.c): the output chosen, the latest
 * value. Rockbox's queues aren't for its other threads, so the tick thread's
 * button_read_device posts it, as it posts the keys. Only the newest matters,
 * so one slot, and a slow tick can't lose the newest. */
static volatile int ev_output = -1; /* 0 DAC, 1 Bluetooth; -1 none pending */
/* The output changed: silent until Rockbox sets the volume that goes with it
 * (misc.c's setvol), not the other output's level meanwhile */
static volatile bool vol_pending;
static volatile bool vol_ack; /* misc.c has swapped in this output's volume */

void shanlingq2_volume_ack(void)
{
    vol_ack = true;
}

bool shanlingq2_take_event(long *id, intptr_t *data)
{
    int o = __atomic_exchange_n(&ev_output, -1, __ATOMIC_ACQ_REL);
    if (o >= 0) {
        *id = SYS_OUTPUT_CHANGED;
        *data = o;
        return true;
    }
    return false;
}
/* DAC ioctls and dac_on, across the tick, main and writer threads. Never from
 * SIGIO, which is why audiohw_mute doesn't take it. */
static pthread_mutex_t dac_mtx = PTHREAD_MUTEX_INITIALIZER;
static volatile bool dac_on;
static volatile long idle_since; /* tick of the last stop; 0: not idle */

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

/* All a DAC powered off has lost, with dac_mtx held. The driver sleeps about
 * 530 ms in here (100 in HEADSET, 428 in POWER), and its last sleep plus the 10
 * jiffies it waits to unmute are settling enough, as for stock. */
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
    bool bt = !headphones_inserted() && bt_connected();
    static int was_bt = -1; /* the kind chosen last time; the first counts as a change */
    bool changed = (int)bt != was_bt;
    if (changed) {
        was_bt = bt;
        vol_pending = true; /* before bt_output: the other output's level stays off this one */
        pcm_set_mixer_volume(-1000, -1000);
    }
    bt_output = bt;
    if (changed) {
        /* misc.c swaps in this output's volume and sets it, which ends the silence.
         * Not on every preinit: a bluealsa rate change reopens too, and each set
         * is a volume popup and a status write. Last: the set relies on bt_output */
        __atomic_store_n(&ev_output, bt, __ATOMIC_RELEASE);
    }
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
    pthread_mutex_lock(&dac_mtx);
    dac_power_on();
    audiohw_mute(false);
    pthread_mutex_unlock(&dac_mtx);
}

static bool dac_idle_expired(void)
{
    long since = idle_since;
    return dac_on && since && !TIME_BEFORE(current_tick, since + DAC_IDLE_TICKS);
}

/* Takes the driver 300 ms when it expires, on the tick thread. The stored mute
 * and volume stay as they are: the driver's mute holds through the wake, which
 * then restores ours.
 * ponytail: ticks pause for those 300 ms, then catch up (kernel-unix.c). Only
 * after 30 s of silence; give it its own thread if the hitch ever shows. */
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
    if (!dac_on && !bt_output)
        dac_power_on();
    pthread_mutex_unlock(&dac_mtx);
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
    pthread_mutex_lock(&dac_mtx);
    audiohw_mute(true);
    dac_set(DAC_POWER, 0);
    dac_on = false;
    if (dac >= 0)
        close(dac);
    dac = -1;
    pthread_mutex_unlock(&dac_mtx);
}

void audiohw_set_frequency(int fsel)
{
    (void)fsel;
}

void audiohw_set_volume(int vol_l, int vol_r)
{
    if (vol_ack) { /* not the sets before misc.c's: the other output's level */
        vol_ack = false;
        vol_pending = false;
    }
    vol[0] = vol_l;
    vol[1] = vol_r;
    if (!muted && !vol_pending)
        pcm_set_mixer_volume(vol_l, vol_r);
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
        pcm_set_mixer_volume(vol[0], vol[1]);
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
