/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2017 Marcin Bukat
 * Copyright (C) 2016 Amaury Pouly
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
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <ucontext.h>
#include <backtrace.h>

#include "system.h"
#include "mv.h"
#include "font.h"
#include "power.h"
#include "button.h"
#include "button-devinput.h"
#include "backlight-target.h"
#include "lcd.h"
#include "filesystem-hosted.h"
#include "logf.h"

/* to make thread-internal.h happy */
uintptr_t *stackbegin;
uintptr_t *stackend;

/* forward-declare */
bool os_file_exists(const char *ospath);

static void sig_handler(int sig, siginfo_t *siginfo, void *context)
{
    /* safe guard variable - we call backtrace() only on first
     * UIE call. This prevent endless loop if backtrace() touches
     * memory regions which cause abort
     */
    static bool triggered = false;

    lcd_set_backdrop(NULL);
    lcd_set_drawinfo(DRMODE_SOLID, LCD_BLACK, LCD_WHITE);
    unsigned line = 0;

    lcd_setfont(FONT_SYSFIXED);
    lcd_set_viewport(NULL);
    lcd_clear_display();

    /* get context info */
    ucontext_t *uc = (ucontext_t *)context;
    unsigned long pc = uc->uc_mcontext.pc;
    unsigned long sp = uc->uc_mcontext.gregs[29];

    lcd_putsf(0, line++, "%s at %08lx", strsignal(sig), pc);

    if(sig == SIGILL || sig == SIGFPE || sig == SIGSEGV || sig == SIGBUS || sig == SIGTRAP) {
        lcd_putsf(0, line++, "address %p", siginfo->si_addr);
    }

    if(!triggered)
    {
        triggered = true;
        rb_backtrace(pc, sp, &line);
    }

#ifdef ROCKBOX_HAS_LOGF
    lcd_putsf(0, line++, "logf:");
    logf_panic_dump(&line);
#endif

    lcd_update();

    system_exception_wait(); /* If this returns, try to reboot */
    system_reboot();
    while (1) {
        // Make sure we're not throttling the cpu
        usleep(1000);
    }
}

#ifdef SHANLING_Q2
/* Rockbox runs under Q2 Pod's boot hook, which starts Q2 Pod for the session
 * after any exit and appends the status to rockbox.log; the next power-on
 * follows its Boot to choice (q2-pod's docs/boot.md, Rockbox). Powering the
 * device off from here needs stock's into_poweroff (MCU and PMIC):
 * /sbin/poweroff alone left the screen blank and the device had to be reset.
 * /sbin/reboot is untried, and the generic code turns the backlight off first.
 * The screen is left on for Q2 Pod, as at boot. */
static void q2_exit_to_launcher(int status)
{
    backlight_hw_on();
    button_close_device();
    sync();
    _exit(status);
}
#endif

void power_off(void)
{
#ifdef SHANLING_Q2
    /* 0x51: "Boot stock OS"; 0: a shutdown */
    q2_exit_to_launcher(q2_boot_stock ? 0x51 : 0);
#endif
    backlight_hw_off();
    button_close_device();
    sync();
    system("/sbin/poweroff");
    while (1) {
        // Make sure we're not throttling the cpu
        usleep(1000);
    }
}

void system_init(void)
{
    int *s;
    /* fake stack, to make thread-internal.h happy */
    stackbegin = stackend = (uintptr_t*)&s;
   /* catch some signals for easier debugging */
    struct sigaction sa;
    sigfillset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = &sig_handler;
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGPIPE, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

void system_reboot(void)
{
#ifdef SHANLING_Q2
    /* After a crash's panic screen, a failed card mount or a Reboot shortcut:
     * Q2 Pod for the session, as for a shutdown; "exit 1" in rockbox.log */
    q2_exit_to_launcher(1);
#endif
    backlight_hw_off();
    system("/sbin/reboot");
    while (1) {
        // Make sure we're not throttling the cpu
        usleep(1000);
    }
}

void system_exception_wait(void)
{
    backlight_hw_on();
    backlight_hw_brightness(DEFAULT_BRIGHTNESS_SETTING);
    /* wait until button press and release */
#ifdef HAVE_BUTTON_DATA
    int bdata;
#define BDATA &bdata
#else
#define BDATA
#endif
    while(button_read_device(BDATA) != 0) {}
    while(button_read_device(BDATA) == 0) {}
    while(button_read_device(BDATA) != 0) {}
    while(button_read_device(BDATA) == 0) {}
}

bool hostfs_removable(IF_MD_NONVOID(int drive))
{
#ifdef HAVE_MULTIDRIVE
    if (drive > 0)
        return true;
    else
#endif
#ifdef HAVE_HOTSWAP_STORAGE_AS_MAIN
        return true;
#else
        return false; /* internal: always present */
#endif
}

bool hostfs_present(IF_MD_NONVOID(int drive))
{
#ifdef HAVE_MULTIDRIVE
    if (drive > 0)
#if defined(MULTIDRIVE_DEV)
        return os_file_exists(MULTIDRIVE_DEV);
#else
        return true; // FIXME?
#endif
    else
#endif
#ifdef HAVE_HOTSWAP_STORAGE_AS_MAIN
        return os_file_exists(ROOTDRIVE_DEV);
#else
        return true; /* internal: always present */
#endif
}

#ifdef HAVE_MULTIDRIVE
int volume_drive(int drive)
{
    return drive;
}
#endif /* HAVE_MULTIDRIVE */

#ifdef CONFIG_STORAGE_MULTI
int hostfs_driver_type(int drive)
{
#if (CONFIG_STORAGE & STORAGE_USB)
    return drive > 0 ? STORAGE_USB_NUM : STORAGE_HOSTFS_NUM;
#else
    return drive > 0 ? STORAGE_SD_NUM : STORAGE_HOSTFS_NUM;
#endif
}
#endif /* CONFIG_STORAGE_MULTI */

int hostfs_init(void)
{
    return 0;
}

int hostfs_flush(void)
{
    sync();
    return 0;
}

#ifdef HAVE_HOTSWAP
bool volume_removable(int volume)
{
    /* don't support more than one partition yet, so volume == drive */
    return hostfs_removable(volume);
}

bool volume_present(int volume)
{
    /* don't support more than one partition yet, so volume == drive */
    return hostfs_present(volume);
}
#endif

int volume_partition(int volume)
{
    (void)volume;
    /* Hosted only implement a single parition per "drive" */
    return 0;
}
