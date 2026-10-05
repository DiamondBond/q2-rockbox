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
#ifndef __LCD_TARGET_H__
#define __LCD_TARGET_H__

/* the panel is portrait: lcd-q2.c turns each update onto it */
#define LCD_OPTIMIZED_UPDATE
#define LCD_OPTIMIZED_UPDATE_RECT

/* lcd-memframe.c's YUV blit lands in Rockbox's own buffer; the next update
 * puts it on the panel */
#define LCD_FRAMEBUF_ADDR(col, row) FBADDR(col, row)

extern void lcd_set_active(bool active);

#endif /* __LCD_TARGET_H__ */
