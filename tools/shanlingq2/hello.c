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

/* M0 probe for the Shanling Q2, run by the dev loader as
 * /mnt/mmc/.rockbox/rockbox. It draws a landscape test card on fb0, logs the
 * platform to stdout (rockbox.log), then every input event for PROBE_S seconds.
 * Upright, the card shows red, green and blue bands from top to bottom with a
 * white square at the top left. */
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define LW 375 /* the landscape view */
#define LH 320
#define PROBE_S 30

static void card(unsigned *fb, unsigned line)
{
    /* landscape (x, y) lies at panel (LH - 1 - y, x): turned clockwise */
    for (int y = 0; y < LH; y++)
        for (int x = 0; x < LW; x++) {
            unsigned c = y < LH / 3 ? 0xffff0000 : y < 2 * LH / 3 ? 0xff00ff00 : 0xff0000ff;
            if (x < 40 && y < 40)
                c = 0xffffffff;
            fb[x * line + LH - 1 - y] = c;
        }
}

int main(void)
{
    setvbuf(stdout, 0, _IOLBF, 0);
    printf("q2 probe\n");
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    int fb = open("/dev/fb0", O_RDWR);
    if (fb < 0 || ioctl(fb, FBIOGET_VSCREENINFO, &var) || ioctl(fb, FBIOGET_FSCREENINFO, &fix)) {
        perror("fb0");
        return 1;
    }
    printf("fb0 %ux%u virtual %ux%u offset %u,%u bpp %u red %u/%u line %u smem %u\n",
           var.xres, var.yres, var.xres_virtual, var.yres_virtual, var.xoffset, var.yoffset,
           var.bits_per_pixel, var.red.offset, var.red.length, fix.line_length, fix.smem_len);
    if (var.xres != LH || var.yres != LW || var.bits_per_pixel != 32 || fix.line_length < LH * 4) {
        printf("unexpected framebuffer, not drawing\n");
    } else {
        unsigned *mem = mmap(0, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
        if (mem == MAP_FAILED) {
            perror("mmap");
            return 1;
        }
        card(mem, fix.line_length / 4);
        var.yoffset = 0;
        if (ioctl(fb, FBIOPAN_DISPLAY, &var))
            perror("pan");
    }

    system("cat /proc/bus/input/devices /proc/asound/cards /proc/asound/pcm; "
           "ls -l /dev/input /dev/shanling_dac /dev/mcu /dev/jz_pwm /sys/class/backlight* "
           "/sys/class/power_supply/battery /sys/devices/i2c-0/0-0062; "
           "cat /sys/class/power_supply/battery/status /sys/devices/i2c-0/0-0062/cw2015_*; "
           "ls /sys/hynitron_debug; mount");

    struct pollfd p[8];
    int ev[8], n = 0;
    for (int i = 0; i < 8; i++) {
        char path[32], name[64] = "";
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;
        ioctl(fd, EVIOCGNAME(sizeof name), name);
        printf("%s: %s\n", path, name);
        p[n].fd = fd;
        p[n].events = POLLIN;
        ev[n++] = i;
    }
    printf("logging input for %d s\n", PROBE_S);
    time_t end = time(0) + PROBE_S;
    while (time(0) < end && poll(p, n, 1000) >= 0)
        for (int i = 0; i < n; i++) {
            struct input_event e;
            if ((p[i].revents & POLLIN) && read(p[i].fd, &e, sizeof e) == sizeof e && e.type)
                printf("%ld.%06ld event%d type %u code %u value %d\n", (long)e.time.tv_sec,
                       (long)e.time.tv_usec, ev[i], e.type, e.code, e.value);
        }
    printf("done\n");
    return 0;
}
