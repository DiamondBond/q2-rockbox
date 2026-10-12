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

/* Shanling Q2 keys and wheel. The keys are "md-gpio-keys" (evdev), the wheel
 * "hyn_sliderbar", a capacitive ring reporting the finger's position as ABS_X
 * and touch as ABS_PRESSURE. A Bluetooth headset's buttons arrive on bluez's
 * "<name> (AVRCP)" uinput device, there only while it is connected. The
 * touchscreen is not used. */
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "button.h"
#include "backlight.h"
#include "kernel.h"
#include "powermgmt.h"
#include "wheel-q2.h"

enum { KEYS, WHEEL, AVRCP, NDEV };
static const char *const names[NDEV] = { "md-gpio-keys", "hyn_sliderbar", "(AVRCP)" };
static struct pollfd fds[NDEV];

static bool is_device(int d, const char *name)
{
    return d == AVRCP ? strstr(name, names[d]) != NULL : !strcmp(name, names[d]);
}

static void open_devices(void)
{
    for (int i = 0; i < 16; i++) {
        char path[32], name[64] = "";
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        ioctl(fd, EVIOCGNAME(sizeof name - 1), name);
        int d = 0;
        while (d < NDEV && (fds[d].fd >= 0 || !is_device(d, name)))
            d++;
        if (d == NDEV) {
            close(fd);
            continue;
        }
        fds[d].fd = fd;
        fds[d].events = POLLIN;
    }
}

void button_init_device(void)
{
    for (int d = 0; d < NDEV; d++)
        fds[d].fd = -1;
    open_devices();

    /* Return held at boot: back to the launcher, which starts the stock
     * player (exit 0x51), so the stock OS stays reachable whatever Rockbox does */
    unsigned char keys[KEY_MAX / 8 + 1] = { 0 };
    if (fds[KEYS].fd >= 0 && ioctl(fds[KEYS].fd, EVIOCGKEY(sizeof keys), keys) >= 0 &&
        (keys[KEY_UP / 8] & (1 << (KEY_UP % 8))))
        _exit(0x51);
}

void button_close_device(void)
{
    for (int d = 0; d < NDEV; d++)
        if (fds[d].fd >= 0)
            close(fds[d].fd), fds[d].fd = -1;
}

static int key_button(int code)
{
    switch (code) {
    case KEY_POWER:        return BUTTON_SELECT; /* the wheel's centre */
    case KEY_UP:           return BUTTON_MENU;   /* Return */
    case KEY_DOWN:         return BUTTON_PLAY;
    case KEY_PREVIOUSSONG: return BUTTON_LEFT;
    case KEY_NEXTSONG:     return BUTTON_RIGHT;
    default:               return 0;
    }
}

/* The headset's buttons, a press each: Rockbox's multimedia keys, which every
 * screen hands to default_event_handler. Play and Pause both toggle, as there
 * is nothing else to send. */
static int headset_button(int code)
{
    switch (code) {
    case KEY_NEXTSONG:     return BUTTON_MULTIMEDIA_NEXT;
    case KEY_PREVIOUSSONG: return BUTTON_MULTIMEDIA_PREV;
    case KEY_PLAYPAUSE:
    case KEY_PLAYCD:
    case KEY_PAUSECD:
    case KEY_PLAY:
    case KEY_PAUSE:
    case KEY_STOPCD:       return BUTTON_MULTIMEDIA_PLAYPAUSE;
    default:               return 0;
    }
}

int button_read_device(void)
{
    static int buttons;
    static int anchor = -1; /* wheel position of the last tick; -1: no finger */
    static long rescan;
    struct input_event e;
    int ticks = 0;

    if (fds[AVRCP].fd < 0 && TIME_AFTER(current_tick, rescan)) {
        rescan = current_tick + HZ;
        open_devices();
    }

    while (poll(fds, NDEV, 0) > 0) {
        for (int d = 0; d < NDEV; d++) {
            if (fds[d].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                close(fds[d].fd);
                fds[d].fd = -1;
                if (d == KEYS) buttons = 0;
                else anchor = -1;
                continue;
            }
            if (!(fds[d].revents & POLLIN) || read(fds[d].fd, &e, sizeof e) != sizeof e)
                continue;
            if (d == KEYS && e.type == EV_KEY) {
                int b = key_button(e.code);
                buttons = e.value ? buttons | b : buttons & ~b;
            } else if (d == AVRCP && e.type == EV_KEY && e.value == 1) {
                int b = headset_button(e.code);
                if (b) {
                    reset_poweroff_timer();
                    button_queue_post(b, 0);
                }
            } else if (d == WHEEL && e.type == EV_ABS) {
                if (e.code == ABS_PRESSURE && !e.value)
                    anchor = -1;
                else if (e.code == ABS_X && anchor < 0)
                    anchor = e.value;
                else if (e.code == ABS_X)
                    ticks += wheel_ticks(&anchor, e.value);
            }
        }
    }

    if (ticks) {
        /* The dark screen's wheel is pocket volume: it adjusts the volume and
         * stays dark, instead of scrolling or waking. The multimedia volume
         * buttons are the portable way there: screens that handle default
         * events adjust the volume, others ignore them. */
        bool dark = !is_backlight_on(false);
        if (!dark)
            backlight_on();
        reset_poweroff_timer();
        int b = ticks * WHEEL_DIR > 0 ?
                (dark ? BUTTON_MULTIMEDIA_VOLUME_UP : BUTTON_SCROLL_FWD) :
                (dark ? BUTTON_MULTIMEDIA_VOLUME_DOWN : BUTTON_SCROLL_BACK);
        for (int n = ticks < 0 ? -ticks : ticks; n > 0; n--)
            button_queue_post(b, 0);
    }
    return buttons;
}
