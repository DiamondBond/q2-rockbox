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
 * fb0 has two pages: an update is drawn on the hidden one, which is then
 * panned to (as stock demo does, so nothing is drawn while it is shown), and
 * copied to the other, to keep both pages the same. */
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
static struct fb_var_screeninfo var;
static int pages = 1, page; /* page: the one on screen */
static bool synced; /* both pages hold the whole frame */

void lcd_init_device(void)
{
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
    if (var.yres_virtual >= 2 * var.yres &&
        fix.smem_len >= 2 * var.yres * fix.line_length)
        pages = 2;
    var.yoffset = 0; /* the page the boot logo is on */
    ioctl(fd, FBIOPAN_DISPLAY, &var);
    lcd_set_active(true);
}

static void draw(int p, int x, int y, int width, int height)
{
    uint32_t *base = panel + p * LCD_WIDTH * stride;
    for (int row = y; row < y + height; row++) {
        const fb_data *src = FBADDR(x, row);
        uint32_t *dst = base + x * stride + (LCD_HEIGHT - 1 - row);
        for (int i = 0; i < width; i++, dst += stride) {
            uint32_t px; /* fb_data is b, g, r, x: the panel's byte order */
            memcpy(&px, &src[i], sizeof px);
            *dst = px | 0xff000000; /* opaque, as display_logo writes */
        }
    }
}

void lcd_update_rect(int x, int y, int width, int height)
{
    if (fd < 0 || !lcd_active())
        return;
    if (!synced) /* the hidden page holds no frame yet */
        x = 0, y = 0, width = LCD_WIDTH, height = LCD_HEIGHT, synced = true;
    if (x < 0)
        width += x, x = 0;
    if (y < 0)
        height += y, y = 0;
    if (x + width > LCD_WIDTH)
        width = LCD_WIDTH - x;
    if (y + height > LCD_HEIGHT)
        height = LCD_HEIGHT - y;
    if (width <= 0 || height <= 0)
        return;

    if (pages == 1) {
        draw(0, x, y, width, height);
        return;
    }
    draw(!page, x, y, width, height);
    page = !page;
    var.yoffset = page * var.yres;
    ioctl(fd, FBIOPAN_DISPLAY, &var);
    unsigned int crtc = 0; /* as stock: wait until the old page is off screen */
    ioctl(fd, FBIO_WAITFORVSYNC, &crtc);
    draw(!page, x, y, width, height);
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
        /* as stock's enable_fb: unblanking alone left the screen black */
        ioctl(fd, FBIOPAN_DISPLAY, &var);
        send_event(LCD_EVENT_ACTIVATION, NULL);
        lcd_update();
    }
}
