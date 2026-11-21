#!/usr/bin/env python3
"""Native checks for button-q2.c's device matching, headset keys and rescans."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
src = (root / 'firmware/target/hosted/shanling/button-q2.c').read_text()


def function(name):
    start = re.search(r'static [^\n]+\b' + name + r'\([^\n]*\)\n\{', src).start()
    return src[start:src.index('\n}\n', start) + 2]


source = r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <linux/input.h>
#define BUTTON_MULTIMEDIA_PLAYPAUSE 1
#define BUTTON_MULTIMEDIA_PREV 2
#define BUTTON_MULTIMEDIA_NEXT 3
''' + src[src.index('enum { KEYS'):src.index('static struct pollfd')] + \
    function('is_device') + function('headset_button') + r'''
int main(void)
{
    assert(is_device(KEYS, "md-gpio-keys") && !is_device(KEYS, "md-gpio-keys2"));
    assert(is_device(AVRCP, "AirPods Pro (AVRCP)") && !is_device(AVRCP, "hyn_ts"));
    assert(headset_button(KEY_NEXTSONG) == BUTTON_MULTIMEDIA_NEXT);
    assert(headset_button(KEY_PREVIOUSSONG) == BUTTON_MULTIMEDIA_PREV);
    assert(headset_button(KEY_PLAYCD) == BUTTON_MULTIMEDIA_PLAYPAUSE);
    assert(headset_button(KEY_PAUSECD) == BUTTON_MULTIMEDIA_PLAYPAUSE);
    assert(headset_button(KEY_VOLUMEUP) == 0);
    return 0;
}
'''

# The whole driver against mocked nodes: event2 is the headset, when it is there.
rescans = r'''
#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/input.h>
#define BUTTON_SELECT 1
#define BUTTON_MENU 2
#define BUTTON_PLAY 4
#define BUTTON_LEFT 8
#define BUTTON_RIGHT 16
#define BUTTON_MULTIMEDIA_VOLUME_UP 32
#define BUTTON_MULTIMEDIA_VOLUME_DOWN 64
#define BUTTON_SCROLL_FWD 128
#define BUTTON_SCROLL_BACK 256
#define BUTTON_MULTIMEDIA_PLAYPAUSE 512
#define BUTTON_MULTIMEDIA_PREV 1024
#define BUTTON_MULTIMEDIA_NEXT 2048
#define HZ 100
#define TIME_AFTER(a, b) ((long)(b) - (long)(a) < 0)
static long current_tick;
static bool bt, hci0;
static const char *nodes[16] = { "md-gpio-keys", "hyn_sliderbar", NULL, "hyn_ts" };
/* What sysfs says instead, when it says something else; unnamed: it can't say */
static const char *renamed[16];
static bool unnamed[16];
static int scans, accesses, opens, closes, lost = -1;
static bool shanlingq2_bt_output(void) { return bt; }
static int mock_access(const char *path, int mode) {
    assert(!strcmp(path, "/sys/class/bluetooth/hci0") && mode == F_OK);
    accesses++;
    return hci0 ? 0 : -1;
}
static bool is_backlight_on(bool ignored) { (void)ignored; return true; }
static void backlight_on(void) {}
static void reset_poweroff_timer(void) {}
static void button_queue_post(int button, int data) { (void)button; (void)data; }
static void q2_key_tone_init(void) {}
static bool sysfs_get_string(const char *path, char *value, int size) {
    int i = -1;
    sscanf(path, "/sys/class/input/event%d/device/name", &i);
    assert(i >= 0 && i < 16);
    if (i == 0) scans++; /* each scan asks event0 first */
    const char *name = renamed[i] ? renamed[i] : nodes[i];
    if (!name || unnamed[i]) return false;
    snprintf(value, size, "%s", name);
    return true;
}
static int mock_open(const char *path, int flags) {
    int i = -1;
    (void)flags;
    sscanf(path, "/dev/input/event%d", &i);
    assert(i >= 0 && i < 16);
    if (!nodes[i]) return -1;
    opens++;
    return 100 + i;
}
static int mock_ioctl(int fd, unsigned long request, void *arg) {
    if (request == EVIOCGNAME(95)) strncpy(arg, nodes[fd - 100], 95);
    return 0;
}
static int mock_close(int fd) { if (fd >= 100) closes++; return 0; }
static int mock_poll(struct pollfd *p, nfds_t n, int timeout) {
    (void)timeout;
    int hit = lost >= 0 && p[lost].fd >= 0;
    for (int i = 0; i < (int)n; i++) p[i].revents = hit && i == lost ? POLLHUP : 0;
    return hit;
}
#define open mock_open
#define ioctl mock_ioctl
#define close mock_close
#define poll mock_poll
#define access mock_access
''' + '#include "firmware/target/hosted/shanling/button-q2.c"\n' + r'''
/* How many scans a stretch of whole seconds took (TIME_AFTER: HZ + 1 ticks each) */
static int scans_in(int seconds) {
    int before = scans;
    for (int i = 0; i < seconds * (HZ + 1); i++) { current_tick++; button_read_device(); }
    return scans - before;
}
/* d's node goes: a scan each second until it is back, then none */
static void lose_and_recover(int d) {
    const char *node = nodes[d];
    nodes[d] = NULL;
    lost = d; current_tick++; button_read_device(); lost = -1;
    assert(fds[d].fd < 0 && scans_in(3) == 3);
    nodes[d] = node;
    assert(scans_in(1) == 1 && fds[d].fd == 100 + d && scans_in(5) == 0);
}
int main(void) {
    /* Cold boot, no Bluetooth: init's scan is the only one, one access() a second */
    current_tick = 1; button_init_device();
    assert(scans == 1 && fds[KEYS].fd == 100 && fds[WHEEL].fd == 101 && fds[AVRCP].fd < 0);
    assert(scans_in(60) == 0 && accesses == 60);
    /* Keys and wheel come back with no Bluetooth */
    lose_and_recover(KEYS);
    lose_and_recover(WHEEL);
    /* Bluetooth output without its headset's node: a scan each second */
    bt = true; assert(scans_in(5) == 5);
    /* The stack up, Bluetooth not the output: a scan each second too, and no
     * node opened or closed to find there is no headset */
    bt = false; hci0 = true;
    opens = closes = 0; assert(scans_in(60) == 60 && opens == 0 && closes == 0);
    /* Neither: a headset's node is left alone */
    hci0 = false; nodes[AVRCP] = "AirPods Pro (AVRCP)";
    assert(scans_in(5) == 0 && fds[AVRCP].fd < 0);
    /* Connected with the wires in, then unplugged: bt_output stays false */
    hci0 = true; assert(scans_in(1) == 1 && fds[AVRCP].fd == 102 && scans_in(5) == 0);
    /* Drops and reconnects after the fallback to the DAC, or on Bluetooth output */
    lose_and_recover(AVRCP);
    hci0 = false; bt = true; lose_and_recover(AVRCP);
    /* Keys come back with the headset held too */
    bt = false; lose_and_recover(KEYS);
    /* A name past 63 characters still ends in "(AVRCP)" */
    hci0 = true;
    nodes[AVRCP] = "A Headset With A Very Long Name That Goes On And On Past 63 (AVRCP)";
    lost = AVRCP; current_tick++; button_read_device(); lost = -1;
    assert(scans_in(1) == 1 && fds[AVRCP].fd == 102);
    /* A node sysfs can't name is opened and asked */
    unnamed[AVRCP] = true;
    lost = AVRCP; current_tick++; button_read_device(); lost = -1;
    assert(scans_in(1) == 1 && fds[AVRCP].fd == 102);
    unnamed[AVRCP] = false;
    /* Renamed between the sysfs read and the open: asked again, and refused */
    renamed[AVRCP] = nodes[AVRCP]; nodes[AVRCP] = "hyn_ts";
    lost = AVRCP; current_tick++; button_read_device(); lost = -1;
    opens = closes = 0;
    assert(scans_in(1) == 1 && fds[AVRCP].fd < 0 && opens == 1 && closes == 1);
    return 0;
}
'''

keymap = (root / 'apps/keymaps/keymap-ipod.c').read_text()
keymap = keymap[keymap.index('button_context_standard[]'):]
q2 = keymap[keymap.index('#ifdef SHANLING_Q2'):keymap.index('#else')]
assert 'BUTTON_MULTIMEDIA_PREV,   BUTTON_LEFT|BUTTON_REL' in q2
assert 'BUTTON_MULTIMEDIA_NEXT,   BUTTON_RIGHT|BUTTON_REL' in q2
assert 'ACTION_STD_CANCEL,        BUTTON_MENU|BUTTON_REL' in q2

with tempfile.TemporaryDirectory() as tmp:
    c, exe = Path(tmp, 't.c'), Path(tmp, 't')
    c.write_text(source)
    subprocess.run(['cc', '-Wall', '-Werror', '-o', exe, c], check=True)
    subprocess.run([exe], check=True)
    for name in ('button.h', 'backlight.h', 'kernel.h', 'powermgmt.h', 'shanlingq2_codec.h', 'sysfs.h'):
        Path(tmp, name).write_text('')
    c.write_text(rescans)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-I', tmp, '-I', root, '-o', exe, c],
                   check=True)
    subprocess.run([exe], check=True)
print('ok')
