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

bool charging_state(void)
{
    static long last_tick;
    static bool charging;
    if (current_tick - last_tick > HZ / 2) {
        char buf[12] = "";
        sysfs_get_string("/sys/class/power_supply/battery/status", buf, sizeof buf);
        last_tick = current_tick;
        charging = !strncmp(buf, "Charging", 8);
    }
    return charging;
}

int _battery_level(void)
{
    int level = 0;
    sysfs_get_int("/sys/devices/i2c-0/0-0062/cw2015_capacity", &level);
    return level;
}

unsigned int power_input_status(void)
{
    return charging_state() ? POWER_INPUT_USB_CHARGER : POWER_INPUT_NONE;
}

bool q2_boot_stock;

/* the headphone amps off, as stock's into_poweroff does through libhardware2's
 * gpio_set_func: { pin, argument count, arguments } */
void q2_amps_off(void)
{
    int fd = open("/dev/gpio", O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return;
    const char *pins[] = { "PC20", "PE22" };
    for (int i = 0; i < 2; i++) {
        const void *arg[3] = { pins[i], (void *)1, "output0" };
        ioctl(fd, 0x20004778, arg);
    }
    close(fd);
}
