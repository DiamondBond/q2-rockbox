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

/* The backlight is PWM channel "PC00" on /dev/jz_pwm. These are the ioctls
 * libhardware2's pwm_request, pwm_config and pwm_set_level make; stock demo's
 * toolSetBackLight sets level + 10 for its brightness 1..100, on a scale of
 * 111. Screen off and on follow stock's screen_action and enable_fb: PWM 0,
 * 50 ms, FBIOBLANK; then FBIOBLANK 0 and a pan, 50 ms, the PWM level. */
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "config.h"
#include "backlight-target.h"
#include "kernel.h"
#include "lcd.h"

#define PWM_REQUEST 0x8001500b /* arg: the pin's name; returns the channel */
#define PWM_CONFIG  0x80185021 /* arg: struct pwm_config */
#define PWM_LEVEL   0xc004502c /* arg: { channel, level } */

static int fd = -1, channel, level = DEFAULT_BRIGHTNESS_SETTING, on = 1;

static void set_pwm(int pwm)
{
    int arg[2] = { channel, pwm };
    if (fd >= 0)
        ioctl(fd, PWM_LEVEL, arg);
}

/* the setting's 1..20 as stock's 5..100 */
static int pwm(void)
{
    return level * 5 + 10;
}

bool backlight_hw_init(void)
{
    /* stock's struct: { 1, 0, 0, 10000, 111, channel }, unnamed */
    int config[6] = { 1, 0, 0, 10000, 111 };
    fd = open("/dev/jz_pwm", O_RDWR | O_CLOEXEC);
    if (fd >= 0 && (channel = ioctl(fd, PWM_REQUEST, "PC00")) >= 0) {
        config[5] = channel;
        ioctl(fd, PWM_CONFIG, config);
        set_pwm(pwm()); /* the screen is already on, from boot */
    } else if (fd >= 0) {
        close(fd), fd = -1;
    }
    return true;
}

void backlight_hw_on(void)
{
    if (on)
        return;
    on = 1;
    lcd_enable(true);
    sleep(HZ / 20);
    set_pwm(pwm());
}

void backlight_hw_off(void)
{
    if (!on)
        return;
    on = 0;
    set_pwm(0);
    sleep(HZ / 20);
    lcd_enable(false);
}

void backlight_hw_brightness(int brightness)
{
    level = MAX(MIN_BRIGHTNESS_SETTING, MIN(brightness, MAX_BRIGHTNESS_SETTING));
    if (on)
        set_pwm(pwm());
}
