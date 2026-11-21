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
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
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

/* Events for Rockbox's main thread (misc.c), the latest value of each: the
 * output chosen, and the headset's volume. Rockbox's queues aren't for its
 * other threads, so the tick thread's button_read_device posts them, as it
 * posts the keys. Only the newest matters, so one slot each, and a slow tick
 * can't lose the newest. */
static volatile int ev_output = -1;      /* 0 DAC, 1 Bluetooth; -1 none pending */
static volatile int ev_volume = INT_MIN; /* the headset's, in dB; INT_MIN none */
/* The output changed: silent until Rockbox sets the volume that goes with it
 * (misc.c's setvol), not the other output's level meanwhile */
static volatile bool vol_pending;
static volatile bool vol_ack; /* misc.c has swapped in this output's volume */

void shanlingq2_volume_ack(void)
{
    vol_ack = true;
}

/* The headset's own volume, through bluez-alsa's ctl "bluealsa": its "<name> -
 * A2DP Playback Volume" element, 0..127, is AVRCP absolute volume, which the
 * headset applies itself (the Q2's bluealsa runs with native volume; Q2 Pod's
 * Control Device Volume relies on that too). While such a headset is the
 * output, Rockbox's volume sets it and pcm-alsa's mixer carries only the
 * balance; the headset's own keys or a car's knob move it, and Rockbox's
 * volume follows (SYS_VOLUME_CHANGED). A thread of its own: a ctl write is a
 * D-Bus round trip. It sleeps in poll() on the ctl's descriptors and a pipe,
 * and nothing wakes it between. A headset without the element keeps the
 * mixer's volume, as before. */
#define DEV_VOL_MAX 127
#define VOL_MIN_CB  (-1000) /* AUDIOHW_SETTING(VOLUME): -100..0 dB, in tenths */
static int dev_pipe[2] = { -1, -1 };
static volatile int dev_want = -1;     /* 0..127 to write; -1 none */
static volatile bool dev_vol;          /* the headset has the element */
static volatile int dev_reported = INT_MIN; /* the cb last reported to Rockbox */

static int cb_to_dev(int cb)
{
    if (cb < VOL_MIN_CB)
        cb = VOL_MIN_CB;
    if (cb > 0)
        cb = 0;
    return ((cb - VOL_MIN_CB) * DEV_VOL_MAX + -VOL_MIN_CB / 2) / -VOL_MIN_CB;
}

static int dev_to_db(int d) /* whole dB: global_status.volume's unit */
{
    int cb = VOL_MIN_CB + (d * -VOL_MIN_CB + DEV_VOL_MAX / 2) / DEV_VOL_MAX;
    return (cb - 5) / 10;
}

/* The mixer: the volume, or with the headset's own volume in use only the
 * balance, the louder channel at 0 dB */
static void apply_mixer(void)
{
    if (muted || vol_pending) {
        pcm_set_mixer_volume(-1000, -1000); /* tenths of a dB */
        return;
    }
    if (bt_output && dev_vol) {
        int m = MAX(vol[0], vol[1]);
        pcm_set_mixer_volume(vol[0] - m, vol[1] - m);
    } else {
        pcm_set_mixer_volume(vol[0], vol[1]);
    }
}

static void dev_kick(void)
{
    if (dev_pipe[1] >= 0) {
        char c = 0;
        ssize_t r = write(dev_pipe[1], &c, 1);
        (void)r;
    }
}

/* The volume element among the ctl's, or false */
static bool dev_find(snd_ctl_t *ctl, snd_ctl_elem_id_t *id)
{
    snd_ctl_elem_list_t *list;
    bool found = false;
    if (snd_ctl_elem_list_malloc(&list) < 0)
        return false;
    if (snd_ctl_elem_list(ctl, list) >= 0 &&
        snd_ctl_elem_list_alloc_space(list, snd_ctl_elem_list_get_count(list)) >= 0 &&
        snd_ctl_elem_list(ctl, list) >= 0) {
        unsigned n = snd_ctl_elem_list_get_used(list);
        for (unsigned i = 0; i < n && !found; i++) {
            const char *name = snd_ctl_elem_list_get_name(list, i);
            size_t len = strlen(name);
            static const char suffix[] = " - A2DP Playback Volume";
            if (len > sizeof suffix - 1 && !strcmp(name + len - (sizeof suffix - 1), suffix)) {
                snd_ctl_elem_list_get_id(list, i, id);
                found = true;
            }
        }
        snd_ctl_elem_list_free_space(list);
    }
    snd_ctl_elem_list_free(list);
    return found;
}

static int dev_read(snd_ctl_t *ctl, snd_ctl_elem_id_t *id, snd_ctl_elem_value_t *v)
{
    snd_ctl_elem_value_set_id(v, id);
    if (snd_ctl_elem_read(ctl, v) < 0)
        return -1;
    return (int)snd_ctl_elem_value_get_integer(v, 0);
}

static bool dev_write(snd_ctl_t *ctl, snd_ctl_elem_id_t *id, snd_ctl_elem_value_t *v, int d)
{
    snd_ctl_elem_value_set_id(v, id);
    snd_ctl_elem_value_set_integer(v, 0, d);
    snd_ctl_elem_value_set_integer(v, 1, d);
    return snd_ctl_elem_write(ctl, v) >= 0;
}

/* The headset's reports, as stock's player takes them (it reads the headset
 * every 400 ms and keeps only what stays): a report counts once its value has
 * held for DEV_HOLD_MS. For DEV_SETTLE_MS after a write of ours the headset is
 * settling: its confirmations come late, in its own steps, and after the
 * hand-over the one for stock's raise to the maximum can arrive after our
 * write. A read-back within one step confirms the write; another value is
 * written over again, twice at most. The first write to a transport is
 * confirmed before the mixer leaves the volume to the headset: until then, and
 * for a headset that holds to its own level, pcm-alsa's mixer keeps it. */
#define DEV_HOLD_MS   400
#define DEV_SETTLE_MS 1500
static volatile bool dev_scan_req; /* the swap set: confirm the headset at this level */

static int64_t dev_now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* Writes the headset's volume and opens the settling window. The reports queued
 * are from before the write and go; an element added or removed meanwhile is
 * kept as a rescan. */
static bool dev_write_settle(snd_ctl_t *ctl, snd_ctl_elem_id_t *id, snd_ctl_elem_value_t *v,
                             snd_ctl_event_t *ev, int d, bool *rescan, int64_t *quiet_until)
{
    if (!dev_write(ctl, id, v, d))
        return false;
    while (snd_ctl_read(ctl, ev) > 0) {
        if (snd_ctl_event_get_type(ev) != SND_CTL_EVENT_ELEM)
            continue;
        unsigned mask = snd_ctl_event_elem_get_mask(ev);
        if (mask == SND_CTL_EVENT_MASK_REMOVE || (mask & SND_CTL_EVENT_MASK_ADD))
            *rescan = true;
    }
    *quiet_until = dev_now_ms() + DEV_SETTLE_MS;
    return true;
}

static void *dev_thread(void *arg)
{
    (void)arg;
    snd_ctl_t *ctl = NULL;
    snd_ctl_elem_id_t *id;
    snd_ctl_elem_value_t *val;
    snd_ctl_event_t *ev;
    int last = -1;  /* what the headset is at, as last written or adopted */
    int cand = -1;  /* a value it reported, waiting to hold */
    int64_t cand_at = 0, quiet_until = 0;
    int retries = 0;
    bool scan = false;  /* the element is to be looked for */
    bool first = false; /* the first write to this transport awaits its confirmation */
    if (snd_ctl_elem_id_malloc(&id) < 0 || snd_ctl_elem_value_malloc(&val) < 0 ||
        snd_ctl_event_malloc(&ev) < 0)
        return NULL;
    for (;;) {
        if (!ctl) {
            if (snd_ctl_open(&ctl, BT_DEVICE, 0) < 0) {
                ctl = NULL; /* no bluealsa: again at the next kick (a swap set) */
                struct pollfd p = { .fd = dev_pipe[0], .events = POLLIN };
                poll(&p, 1, -1);
                char b[16];
                while (read(dev_pipe[0], b, sizeof b) > 0)
                    ;
                continue;
            }
            snd_ctl_subscribe_events(ctl, 1);
            scan = true;
        }
        if (scan) {
            scan = false;
            __atomic_store_n(&dev_scan_req, false, __ATOMIC_RELEASE); /* this is it */
            last = cand = -1; /* a new transport: nothing known about it */
            quiet_until = 0;
            retries = 0;
            first = false;
            dev_vol = false; /* the mixer's volume until the headset confirms ours */
            apply_mixer();
            /* not while the output's level is still to be swapped in: its set requests this again */
            if (dev_find(ctl, id) && bt_output && !vol_pending) {
                int d = cb_to_dev(MAX(vol[0], vol[1]));
                if (dev_write_settle(ctl, id, val, ev, d, &scan, &quiet_until)) {
                    last = d;
                    first = true;
                }
            }
            if (scan)
                continue; /* the element came or went during the write */
        }
        int64_t end = first ? quiet_until : cand >= 0 ? MAX(cand_at + DEV_HOLD_MS, quiet_until) : -1;
        int tmo = -1; /* only while a write awaits its confirmation or a report its hold */
        if (end >= 0) {
            int64_t left = end - dev_now_ms();
            tmo = left > 0 ? (int)left : 0;
        }
        struct pollfd pfd[4];
        int np = snd_ctl_poll_descriptors(ctl, pfd, 3);
        if (np < 0)
            np = 0;
        pfd[np].fd = dev_pipe[0];
        pfd[np].events = POLLIN;
        int r = poll(pfd, np + 1, tmo);
        if (r < 0)
            continue;
        if (pfd[np].revents & POLLIN) {
            char b[16];
            while (read(dev_pipe[0], b, sizeof b) > 0)
                ;
            if (__atomic_exchange_n(&dev_scan_req, false, __ATOMIC_ACQ_REL))
                scan = true; /* the swap set: confirm the headset at this level */
            int w = __atomic_exchange_n(&dev_want, -1, __ATOMIC_ACQ_REL);
            if (w >= 0 && dev_vol && w != last &&
                dev_write_settle(ctl, id, val, ev, w, &scan, &quiet_until)) {
                last = w;
                retries = 0;
                cand = -1;
            }
            if (scan)
                continue;
        }
        int64_t now = dev_now_ms();
        if (first && now >= quiet_until) {
            /* the first write: the level Rockbox wants now, if it moved meanwhile, is
             * written instead; else a read-back within one step confirms it, another
             * value is written over again, twice at most, or the headset holds to its
             * own level and the mixer keeps the volume */
            bool live = bt_output && !vol_pending; /* read before vol[]: it is theirs */
            int want = live ? cb_to_dev(MAX(vol[0], vol[1])) : last;
            if (abs(want - last) > 1 &&
                dev_write_settle(ctl, id, val, ev, want, &scan, &quiet_until)) {
                last = want;
                retries = 0;
                continue;
            }
            int d = dev_read(ctl, id, val);
            bool ok = d >= 0 && abs(d - want) <= 1;
            if (!ok && retries < 2 &&
                dev_write_settle(ctl, id, val, ev, want, &scan, &quiet_until)) {
                last = want;
                retries++;
                continue;
            }
            first = false;
            retries = 0;
            if (d >= 0)
                last = d;
            if (ok && live) {
                dev_vol = true;
                apply_mixer();
            }
        } else if (!first && cand >= 0 && now >= MAX(cand_at + DEV_HOLD_MS, quiet_until)) {
            int64_t at = cand_at;
            cand = -1;
            int d = dev_read(ctl, id, val);
            if (d >= 0 && d != last) {
                if (abs(d - last) <= 1) {
                    last = d; /* its step for ours */
                } else if (at < quiet_until && retries < 2 && last >= 0 &&
                           dev_write_settle(ctl, id, val, ev, last, &scan, &quiet_until)) {
                    retries++; /* reported while settling: once more */
                    continue;
                } else {
                    last = d; /* its keys or a car's knob: Rockbox follows, no write back */
                    retries = 0;
                    if (bt_output) {
                        int db = dev_to_db(d);
                        dev_reported = db * 10; /* as audiohw_set_volume gets it */
                        __atomic_store_n(&ev_volume, db, __ATOMIC_RELEASE);
                    }
                }
            }
        }
        unsigned short re = 0;
        if (np > 0)
            snd_ctl_poll_descriptors_revents(ctl, pfd, np, &re);
        if (re & (POLLERR | POLLHUP | POLLNVAL)) {
            snd_ctl_close(ctl);
            ctl = NULL;
            dev_vol = false;
            first = false;
            cand = -1;
            apply_mixer();
            continue;
        }
        if (!(re & POLLIN))
            continue;
        bool changed = false;
        while (snd_ctl_read(ctl, ev) > 0) {
            if (snd_ctl_event_get_type(ev) != SND_CTL_EVENT_ELEM)
                continue;
            unsigned mask = snd_ctl_event_elem_get_mask(ev);
            if (mask == SND_CTL_EVENT_MASK_REMOVE || (mask & SND_CTL_EVENT_MASK_ADD))
                scan = true;
            else if (mask & SND_CTL_EVENT_MASK_VALUE)
                changed = true;
        }
        if (scan)
            continue; /* found again above, on the new transport */
        if (changed && dev_vol) {
            int d = dev_read(ctl, id, val);
            if (d >= 0 && d != last && d != cand) {
                cand = d; /* counts once it has held (above) */
                cand_at = now;
            } else if (d == last) {
                cand = -1;
            }
        }
    }
    return NULL;
}

/* With the first Bluetooth output; signals blocked as for the other threads */
static void dev_thread_start(void)
{
    if (dev_pipe[0] >= 0 || pipe2(dev_pipe, O_CLOEXEC | O_NONBLOCK) < 0)
        return;
    sigset_t all, old;
    pthread_t t;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    if (pthread_create(&t, NULL, dev_thread, NULL) != 0) {
        close(dev_pipe[0]);
        close(dev_pipe[1]);
        dev_pipe[0] = dev_pipe[1] = -1;
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);
}

bool shanlingq2_take_event(long *id, intptr_t *data)
{
    int o = __atomic_exchange_n(&ev_output, -1, __ATOMIC_ACQ_REL);
    if (o >= 0) {
        *id = SYS_OUTPUT_CHANGED;
        *data = o;
        return true;
    }
    int v = __atomic_exchange_n(&ev_volume, INT_MIN, __ATOMIC_ACQ_REL);
    if (v != INT_MIN) {
        *id = SYS_VOLUME_CHANGED;
        *data = v;
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
        dev_reported = INT_MIN; /* a headset back may stand elsewhere */
        dev_vol = false;        /* the mixer's volume until dev_thread confirms ours */
    }
    bt_output = bt;
    if (bt)
        dev_thread_start(); /* before the event: its swap set kicks the thread */
    if (changed) {
        /* misc.c swaps in this output's volume and sets it, which ends the silence.
         * Not on every preinit: a bluealsa rate change reopens too, and each set
         * is a volume popup and a status write. Last: the set relies on bt_output */
        __atomic_store_n(&ev_output, bt, __ATOMIC_RELEASE);
    }
    apply_mixer(); /* silent, or the headset's volume, or the mixer's */
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
    vol[0] = vol_l;
    vol[1] = vol_r;
    if (vol_ack) { /* not the sets before misc.c's: the other output's level */
        vol_ack = false;
        vol_pending = false;
        if (bt_output) { /* this output's level: the headset is confirmed at it */
            __atomic_store_n(&dev_scan_req, true, __ATOMIC_RELEASE);
            dev_kick();
        }
    }
    if (bt_output && dev_vol) {
        /* the headset's own volume; not when this is it, reported back */
        int m = MAX(vol_l, vol_r);
        if (m != dev_reported) {
            dev_want = cb_to_dev(m);
            dev_kick();
        }
        dev_reported = INT_MIN;
    }
    apply_mixer();
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
    apply_mixer();
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
