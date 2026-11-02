#!/usr/bin/env python3
"""Native checks for keytone-q2.c: Q2 Pod's settings, the gate and the MCU's commands."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]

source = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
static bool jack, bt;
bool headphones_inserted(void) { return jack; }
bool shanlingq2_bt_output(void) { return bt; }
static const char *config;
static FILE *mock_fopen(const char *path, const char *mode)
{
    assert(!strcmp(path, "/mnt/data/config.ini") && !strcmp(mode, "re"));
    return config ? fmemopen((void *)config, strlen(config), "r") : NULL;
}
#define fopen mock_fopen
static char calls[512];
static volatile int spawns, hold;
static int mock_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *io,
                      const posix_spawnattr_t *attr, char *const argv[], char *const env[])
{
    (void)pid; (void)io; (void)attr; (void)env;
    assert(!strcmp(path, "/usr/bin/cmd_mcu") && !strcmp(argv[0], "cmd_mcu"));
    strcat(calls, argv[1]);
    if (argv[2]) { strcat(calls, " "); strcat(calls, argv[2]); }
    strcat(calls, ";");
    spawns++;
    while (hold) usleep(100);
    return 0;
}
#define posix_spawn mock_spawn
static pid_t mock_waitpid(pid_t p, int *s, int o) { (void)s; (void)o; return p; }
#define waitpid mock_waitpid
static int mock_access(const char *p, int m) { (void)p; (void)m; return 0; }
#define access mock_access
#include "firmware/target/hosted/shanling/keytone-q2.c"

#define INIT "write_firmware /usr/data/libmcu-bare.bin;bootup;reset;"
static void wait_spawns(int n) { while (spawns < n || __atomic_load_n(&tone_busy, __ATOMIC_ACQUIRE)) usleep(100); }

int main(int argc, char **argv)
{
    assert(argc == 5);
    config = strcmp(argv[1], "none") ? argv[1] : NULL;
    jack = argv[2][0] == '1';
    bt = argv[3][0] == '1';
    int clicks = atoi(argv[4]);
    q2_key_tone_init();
    if (clicks < 0) { /* Key Tone off: no thread, no MCU, no click */
        q2_key_tone();
        assert(!q2_key_tone_on && !spawns);
        return 0;
    }
    assert(q2_key_tone_on);
    wait_spawns(3);
    assert(!strcmp(calls, INIT));
    q2_key_tone();
    if (!clicks) {
        assert(__atomic_load_n(&tone_busy, __ATOMIC_ACQUIRE) == 0);
        assert(!strcmp(calls, INIT));
        return 0;
    }
    wait_spawns(4);
    assert(!strcmp(calls, INIT "write_str buzzer;"));
    /* a click while the last one sounds is dropped, the next one plays */
    hold = 1;
    q2_key_tone();
    while (spawns < 5) usleep(100);
    q2_key_tone();
    q2_key_tone();
    hold = 0;
    wait_spawns(5);
    q2_key_tone();
    wait_spawns(6);
    assert(spawns == 6);
    return 0;
}
'''

ON, HP = 'KEYTONE=1\n', 'KEYTONEHP=1\n'
cases = [
    # config, jack, Bluetooth, -1: off; 0: on and gated; 1: clicks
    ('none', 0, 0, -1), ('', 0, 0, -1), ('[SYSSET]\nKEYTONE=0\n', 0, 0, -1),
    ('[SYSSET]\nKEYTONE=x\n', 0, 0, -1), ('[IPOD]\nKEYTONEHP=1\n', 0, 0, -1),
    (f'[SYSSET]\n{ON}', 0, 0, 1), (f'[SYSSET]\r\nKEYTONE=1\r\n', 0, 0, 1),
    (f'[SYSSET]\n{ON}', 1, 0, 0), (f'[SYSSET]\n{ON}', 0, 1, 0),
    (f'[SYSSET]\n{ON}[IPOD]\nKEYTONEHP=0\n', 1, 0, 0),
    (f'[SYSSET]\n{ON}[IPOD]\nKEYTONEHP=2\n', 1, 0, 0),
    (f'[IPOD]\n{HP}[SYSSET]\n{ON}', 1, 0, 1), (f'[SYSSET]\n{ON}[IPOD]\n{HP}', 0, 1, 1),
    (f'[SYSSET]\n{ON}[IPOD]\n{HP}', 0, 0, 1),
]

with tempfile.TemporaryDirectory() as t:
    tmp = Path(t)
    (tmp / 'button.h').write_text('bool headphones_inserted(void);\n')
    (tmp / 'system.h').write_text('extern bool q2_key_tone_on;\nvoid q2_key_tone_init(void);\nvoid q2_key_tone(void);\n')
    (tmp / 'shanlingq2_codec.h').write_text('bool shanlingq2_bt_output(void);\n')
    (tmp / 'test.c').write_text(source)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-g', f'-I{tmp}', f'-I{root}',
                    f'-I{root}/firmware/target/hosted/shanling', str(tmp / 'test.c'),
                    '-pthread', '-o', str(tmp / 'test')], check=True)
    for config, jack, bt, clicks in cases:
        subprocess.run([str(tmp / 'test'), config, str(jack), str(bt), str(clicks)], check=True, timeout=30)
print('keytone ok')
