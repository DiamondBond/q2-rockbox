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
#ifndef _WHEEL_Q2_H_
#define _WHEEL_Q2_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The wheel reports the finger's position around the ring, 1..WHEEL_RANGE. */
#define WHEEL_RANGE 200
/* Anchors and steps count sixteenths of a ring unit, so every sensitivity differs */
#define WHEEL_FINE  16
/* Ring units per tick at Q2 Pod's default Wheel Sensitivity, 50% (its
 * WHEEL_TICK_UNITS, a seventh of a turn) */
#define WHEEL_TICK_UNITS 28
/* 1 if clockwise raises the position, -1 if it lowers it: on the device,
 * clockwise lowers it */
#define WHEEL_DIR   -1

/* A setting Q2 Pod saved in config.ini (NULL: none), held in line; "" if missing */
static inline const char *q2pod_value(FILE *config, const char *key, char *line, int size)
{
    size_t n = strlen(key);
    if (config)
        rewind(config);
    while (config && fgets(line, size, config)) {
        if (!strncmp(line, key, n) && line[n] == '=') {
            line[n + 1 + strcspn(line + n + 1, "\r\n")] = '\0';
            return line + n + 1;
        }
    }
    return "";
}

/* The step for config.ini: Q2 Pod's Wheel Sensitivity is the calibration knob
 * for how far a turn scrolls in both systems. As Q2 Pod reads it (wheel_pct),
 * anything but a canonical ten in 50..200 is 50%. */
static inline int wheel_read_step(FILE *config)
{
    char line[256], canonical[16];
    const char *v = q2pod_value(config, "WHEELSENSITIVITY", line, sizeof line);
    int pct = 50, n = atoi(v);
    snprintf(canonical, sizeof canonical, "%d", n);
    if (n >= 50 && n <= 200 && !(n % 10) && !strcmp(v, canonical))
        pct = n;
    return (WHEEL_TICK_UNITS * WHEEL_FINE * 50 + pct / 2) / pct;
}

/* Whole ticks for the finger's move from *anchor (in WHEEL_FINE) to position x,
 * the shorter way round (positive: position rising). *anchor advances by the
 * ticks taken, so the remainder carries over and jitter at a step's edge does
 * not tick back. */
static inline int wheel_ticks(int *anchor, int x, int step)
{
    const int range = WHEEL_RANGE * WHEEL_FINE;
    int d = x * WHEEL_FINE - *anchor;
    if (d > range / 2)
        d -= range;
    else if (d < -range / 2)
        d += range;
    int t = d / step;
    *anchor += t * step;
    if (*anchor > range)
        *anchor -= range;
    else if (*anchor < 1)
        *anchor += range;
    return t;
}

#endif /* _WHEEL_Q2_H_ */
