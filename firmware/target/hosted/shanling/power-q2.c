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

/* Charging from the AXP2101 PMIC, the level from the cw2015 gauge (the PMIC's
 * power_supply has no capacity on the Q2). For USB storage, shut down to the
 * stock player: dwc2's pc_link stays 0 until it sets up the gadget. */
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "system.h"
#include "kernel.h"
#include "power.h"
#include "sysfs.h"

static bool charging, plugged;

static void read_charger(void)
{
    static long last_tick;
    static bool sampled;
    if (!sampled || (unsigned long)current_tick - (unsigned long)last_tick >= HZ) {
        char buf[16];
        sampled = true;
        last_tick = current_tick;
        if (!sysfs_get_string("/sys/class/power_supply/battery/status", buf, sizeof buf))
            return;
        if (!strcmp(buf, "Charging") || !strcmp(buf, "Full")) {
            charging = !strcmp(buf, "Charging");
            plugged = true;
        } else if (!strcmp(buf, "Discharging") || !strcmp(buf, "Not charging")) {
            charging = plugged = false;
        }
    }
}

bool charging_state(void)
{
    read_charger();
    return charging;
}

int _battery_level(void)
{
    static int last_level = -1;
    int level;
    if (sysfs_get_int("/sys/devices/i2c-0/0-0062/cw2015_capacity", &level) &&
        level >= 0 && level <= 100)
        last_level = level;
    return last_level;
}

unsigned int power_input_status(void)
{
    read_charger();
    return plugged ? POWER_INPUT_USB_CHARGER : POWER_INPUT_NONE;
}

bool q2_boot_stock;

/* /dev/gpio, as libhardware2's gpio_get_value uses it */
#define GPIO_GET_VALUE 0x2000477a /* arg: the pin's name; returns its level */

static int gpio(void)
{
    static int fd = -1;
    if (fd < 0)
        fd = open("/dev/gpio", O_RDWR | O_CLOEXEC);
    return fd;
}

/* the 3.5 mm (PA07) and 4.4 mm (PA08) jacks, 1 = plugged, as stock's
 * check_headset_status reads them */
bool headphones_inserted(void)
{
    return gpio() >= 0 && (ioctl(gpio(), GPIO_GET_VALUE, "PA07") > 0 ||
                           ioctl(gpio(), GPIO_GET_VALUE, "PA08") > 0);
}
