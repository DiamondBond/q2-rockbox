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

/* Q2 Pod's Key Tone, on the MCU's buzzer: the Q2's own speaker, never the
 * headphones. Rockbox follows Q2 Pod's settings, read at startup: Key Tone, and
 * Click With Headphones, which keeps it clicking while headphones or Bluetooth
 * listen. Rockbox's own Keyclick setting is a separate beep through the audio
 * output. */
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <sys/wait.h>
#include <unistd.h>
#include "button.h"
#include "system.h"
#include "shanlingq2_codec.h"
#include "wheel-q2.h"

bool q2_key_tone_on;
static bool with_headphones;
static int tone_busy = 1; /* and until the MCU is up */
static int tone_click[2]; /* a pipe: a byte for each click */
static posix_spawn_file_actions_t quiet;

/* Waits for stock's tool, its output kept out of rockbox.log. posix_spawn, as
 * a fork would stop to copy Rockbox's memory on every click during playback. */
static void cmd_mcu(char *arg, char *arg2)
{
    char *const argv[] = { "cmd_mcu", arg, arg2, NULL };
    pid_t pid;
    if (!posix_spawn(&pid, "/usr/bin/cmd_mcu", &quiet, NULL, argv, environ))
        waitpid(pid, NULL, 0);
}

/* The tool waits out the buzzer, so the clicks run here, not on the UI thread */
static void *tone_worker(void *arg)
{
    (void)arg;
    /* Stock's buzzer_init: nothing else has started the MCU at power-on. From
     * Q2 Pod's Home it is already up, and demo repeats this when it starts again. */
    for (int i = 0; i < 40 && access("/dev/mcu", F_OK); i++)
        usleep(50000); /* the driver loads in S11; stock waits up to 2 s */
    cmd_mcu("write_firmware", "/usr/data/libmcu-bare.bin");
    cmd_mcu("bootup", NULL);
    cmd_mcu("reset", NULL);
    for (;;) {
        char byte;
        __atomic_store_n(&tone_busy, 0, __ATOMIC_RELEASE);
        if (read(tone_click[0], &byte, 1) == 1)
            cmd_mcu("write_str", "buzzer");
    }
    return NULL;
}

void q2_key_tone_init(void)
{
    char line[256];
    FILE *config = fopen("/mnt/data/config.ini", "re");
    q2_key_tone_on = !strcmp(q2pod_value(config, "KEYTONE", line, sizeof line), "1");
    with_headphones = !strcmp(q2pod_value(config, "KEYTONEHP", line, sizeof line), "1");
    if (config)
        fclose(config);
    posix_spawn_file_actions_init(&quiet);
    posix_spawn_file_actions_addopen(&quiet, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_adddup2(&quiet, 1, 2);
    if (!q2_key_tone_on) {
        /* From Q2 Pod's Home the MCU is up, as demo's buzzer_init starts it
         * whatever Key Tone says, polling for a click that won't come. This holds
         * it in reset: one register bit in the driver, already set at power-on,
         * and demo's buzzer_init releases it when it next starts. */
        cmd_mcu("shutdown", NULL);
        return;
    }
    if (pipe2(tone_click, O_CLOEXEC))
        return;

    sigset_t all, old;
    pthread_t worker;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    /* if it fails, tone_busy stays set and the Q2 stays silent */
    if (!pthread_create(&worker, NULL, tone_worker, NULL))
        pthread_detach(worker);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
}

void q2_key_tone(void)
{
    if (!q2_key_tone_on)
        return;
    if (!with_headphones && (headphones_inserted() || shanlingq2_bt_output()))
        return;
    /* a click while the last one still sounds is dropped, as in Q2 Pod */
    if (!__atomic_exchange_n(&tone_busy, 1, __ATOMIC_ACQUIRE) && write(tone_click[1], "", 1) != 1)
        __atomic_store_n(&tone_busy, 0, __ATOMIC_RELEASE);
}
