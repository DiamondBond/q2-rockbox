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

/* The Q2's /dev/fb0 is the 320x375 portrait panel, 32 bpp with red at bit 16.
 * Rockbox draws 375x320 landscape; each update is turned clockwise onto it,
 * landscape (x, y) at panel (319 - y, x), as the stock boot logo is stored.
 * ponytail: one framebuffer page, written in place, so a fast redraw may tear;
 * draw the other page and pan (as stock demo does) if it shows. */
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "lcd.h"
#include "lcd-target.h"
#include "panic.h"

static int fd = -1;
static uint32_t *panel;
static unsigned stride; /* panel pixels per row */

void lcd_init_device(void)
{
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &var) ||
        ioctl(fd, FBIOGET_FSCREENINFO, &fix))
        panicf("Cannot open /dev/fb0");
    if (var.xres != LCD_HEIGHT || var.yres != LCD_WIDTH || var.bits_per_pixel != 32 ||
        fix.line_length < LCD_HEIGHT * 4)
        panicf("Unexpected framebuffer %ux%u, %u bpp", var.xres, var.yres,
               var.bits_per_pixel);
    stride = fix.line_length / 4;
    panel = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (panel == MAP_FAILED)
        panicf("Cannot map framebuffer");
    var.yoffset = 0; /* the page the boot logo is on */
    ioctl(fd, FBIOPAN_DISPLAY, &var);
    lcd_set_active(true);
}

void lcd_update_rect(int x, int y, int width, int height)
{
    if (fd < 0 || !lcd_active())
        return;
    if (x < 0)
        width += x, x = 0;
    if (y < 0)
        height += y, y = 0;
    if (x + width > LCD_WIDTH)
        width = LCD_WIDTH - x;
    if (y + height > LCD_HEIGHT)
        height = LCD_HEIGHT - y;

    for (int row = y; row < y + height; row++) {
        const fb_data *src = FBADDR(x, row);
        uint32_t *dst = panel + x * stride + (LCD_HEIGHT - 1 - row);
        for (int i = 0; i < width; i++, dst += stride) {
            uint32_t p; /* fb_data is b, g, r, x: the panel's byte order */
            memcpy(&p, &src[i], sizeof p);
            *dst = p | 0xff000000; /* opaque, as display_logo writes */
        }
    }
}

void lcd_update(void)
{
    lcd_update_rect(0, 0, LCD_WIDTH, LCD_HEIGHT);
}

void lcd_enable(bool on)
{
    if (fd < 0 || lcd_active() == on)
        return;
    lcd_set_active(on);
    ioctl(fd, FBIOBLANK, on ? FB_BLANK_UNBLANK : FB_BLANK_POWERDOWN);
    if (on) {
        send_event(LCD_EVENT_ACTIVATION, NULL);
        lcd_update();
    }
}
