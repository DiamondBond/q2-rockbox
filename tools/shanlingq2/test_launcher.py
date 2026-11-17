#!/usr/bin/env python3
"""Host check for how Rockbox leaves the Q2 (requires cc).

Rockbox runs under Q2 Pod's boot hook, which starts Q2 Pod after any exit and
appends the status to rockbox.log (q2-pod's docs/boot.md, Rockbox). Shutting
down and rebooting both go back there with the screen on, never through
/sbin/poweroff or /sbin/reboot; other hosted targets keep those."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
src = (root / 'firmware/target/hosted/system-hosted.c').read_text()


def function(signature):
    start = src.index(signature)
    return src[start:src.index('\n}\n', start) + 3]


# power_off and what sits between it and sig_handler, then system_reboot
start = src.index('\n}\n', src.index('static void sig_handler')) + 3
exits = src[start:src.index('void system_init(void)')] + function('void system_reboot(void)')

source = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
static jmp_buf done;
static int status, light, closed, synced;
static const char *command;
bool q2_boot_stock;
void backlight_hw_on(void) { light = 1; }
void backlight_hw_off(void) { light = 0; }
void button_close_device(void) { closed++; }
void mock_sync(void) { synced++; }
void mock_exit(int s) { status = s; longjmp(done, 1); }
int mock_system(const char *c) { command = c; return 0; }
int mock_usleep(unsigned us) { (void)us; longjmp(done, 1); } /* the loop after it */
#define sync mock_sync
#define _exit mock_exit
#define system mock_system
#define usleep mock_usleep
''' + exits + r'''
static void run(void (*f)(void))
{
    status = light = -1;
    closed = synced = 0;
    command = NULL;
    if (!setjmp(done))
        f();
}

int main(void)
{
#ifdef SHANLING_Q2
    /* a shutdown, then "Boot stock OS" */
    run(power_off);
    assert(status == 0 && light == 1 && closed == 1 && synced == 1 && !command);
    q2_boot_stock = true;
    run(power_off);
    assert(status == 0x51 && light == 1 && closed == 1 && synced == 1 && !command);
    q2_boot_stock = false;
    /* after a crash's panic screen, a failed mount or a Reboot shortcut */
    run(system_reboot);
    assert(status == 1 && light == 1 && closed == 1 && synced == 1 && !command);
#else
    run(power_off);
    assert(status == -1 && light == 0 && !strcmp(command, "/sbin/poweroff"));
    run(system_reboot);
    assert(status == -1 && light == 0 && !strcmp(command, "/sbin/reboot"));
#endif
    return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    (tmp / 'test.c').write_text(source)
    for define in ('-DSHANLING_Q2', '-UOTHER_TARGET'):
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', define,
                        str(tmp / 'test.c'), '-o', str(tmp / 'test')], check=True)
        subprocess.run([str(tmp / 'test')], check=True)
print('Q2 shutdown, Boot stock OS and reboot to the launcher; others unchanged: OK')
