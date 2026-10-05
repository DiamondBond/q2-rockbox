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
#ifndef _BUTTON_TARGET_H_
#define _BUTTON_TARGET_H_

/* The iPod's buttons (IPOD_4G_PAD): Return is Menu, the side buttons are
 * Left and Right */
#define BUTTON_SELECT       0x00000001
#define BUTTON_MENU         0x00000002
#define BUTTON_LEFT         0x00000004
#define BUTTON_RIGHT        0x00000008
#define BUTTON_SCROLL_FWD   0x00000010
#define BUTTON_SCROLL_BACK  0x00000020
#define BUTTON_PLAY         0x00000040

#define BUTTON_MAIN (BUTTON_SELECT|BUTTON_MENU|BUTTON_LEFT|BUTTON_RIGHT|\
                     BUTTON_SCROLL_FWD|BUTTON_SCROLL_BACK|BUTTON_PLAY)

/* No hold switch: both side buttons held at boot clear the settings */
#define SETTINGS_RESET (BUTTON_LEFT|BUTTON_RIGHT)

/* Software power-off */
#define POWEROFF_BUTTON BUTTON_PLAY
#define POWEROFF_COUNT  40

void button_close_device(void);

#endif /* _BUTTON_TARGET_H_ */
