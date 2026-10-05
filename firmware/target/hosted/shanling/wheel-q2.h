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

/* The wheel reports the finger's position around the ring, 1..WHEEL_RANGE. */
#define WHEEL_RANGE 200
/* Ring units per tick: 20 ticks a turn (stock: 20 for a gesture's first, then
 * 10). The calibration knob for how far a turn scrolls. */
#define WHEEL_STEP  10
/* 1 if clockwise raises the position, -1 if it lowers it: on the device,
 * clockwise lowers it */
#define WHEEL_DIR   -1

/* Whole ticks for the finger's move from *anchor to x, the shorter way round
 * (positive: position rising). *anchor advances by the ticks taken, so the
 * remainder carries over and jitter at a step's edge does not tick back. */
static inline int wheel_ticks(int *anchor, int x)
{
    int d = x - *anchor;
    if (d > WHEEL_RANGE / 2)
        d -= WHEEL_RANGE;
    else if (d < -WHEEL_RANGE / 2)
        d += WHEEL_RANGE;
    int t = d / WHEEL_STEP;
    *anchor += t * WHEEL_STEP;
    if (*anchor > WHEEL_RANGE)
        *anchor -= WHEEL_RANGE;
    else if (*anchor < 1)
        *anchor += WHEEL_RANGE;
    return t;
}

#endif /* _WHEEL_Q2_H_ */
