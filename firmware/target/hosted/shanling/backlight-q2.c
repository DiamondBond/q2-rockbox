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

/* The backlight is on from boot and stays on. ponytail: FBIOBLANK alone left
 * the screen black on wake (stock also resets the PWM backlight and pans);
 * screen-off comes with the PWM backlight on PC00 in M3 (tools/shanlingq2/README). */
#include "config.h"
#include "backlight-target.h"

bool backlight_hw_init(void)
{
    backlight_hw_on();
    return true;
}

void backlight_hw_on(void)
{
}

void backlight_hw_off(void)
{
}

void backlight_hw_brightness(int brightness)
{
    (void)brightness;
}
