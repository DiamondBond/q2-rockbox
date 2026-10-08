#!/usr/bin/env python3
"""Compile Q2 display and battery drivers against mocked hardware boundaries."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as directory:
    tmp = Path(directory)
    for header in ('system.h', 'kernel.h', 'power.h', 'lcd.h', 'panic.h'):
        (tmp / header).write_text('')
    display = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <linux/fb.h>
#define LCD_WIDTH 375
#define LCD_HEIGHT 320
#define LCD_EVENT_ACTIVATION 1
typedef uint32_t fb_data;
static fb_data frame[LCD_HEIGHT][LCD_WIDTH];
static unsigned rows, pans, waits;
static bool active = true, fail_pan;
static const fb_data *address(int x, int y) { rows++; return &frame[y][x]; }
#define FBADDR(x,y) address(x,y)
static bool lcd_active(void) { return active; }
void lcd_set_active(bool on) { active = on; }
static void send_event(int id, void *p) { (void)id; (void)p; }
static void panicf(const char *s, ...) { (void)s; abort(); }
#define ioctl mock_ioctl
#include "firmware/target/hosted/shanling/lcd-q2.c"
int mock_ioctl(int dev, unsigned long request, ...) {
    assert(dev == 42);
    if (request == FBIOPAN_DISPLAY) { pans++; return fail_pan ? -1 : 0; }
    if (request == FBIO_WAITFORVSYNC) waits++;
    return 0;
}
static void verify(void) {
    for (int y = 0; y < LCD_HEIGHT; y++)
        for (int x = 0; x < LCD_WIDTH; x++)
            assert(panel[page * LCD_WIDTH * stride + x * stride + LCD_HEIGHT - 1 - y]
                   == (frame[y][x] | 0xff000000));
}
int main(void) {
    fd = 42; pages = 2; stride = LCD_HEIGHT + 4; var.yres = LCD_WIDTH;
    panel = calloc(2 * LCD_WIDTH * stride, sizeof(*panel)); assert(panel);
    for (int y = 0; y < LCD_HEIGHT; y++)
        for (int x = 0; x < LCD_WIDTH; x++) frame[y][x] = y * LCD_WIDTH + x;
    lcd_update(); verify(); assert(rows == LCD_HEIGHT && page == 1);
    rows = 0; lcd_update(); verify(); assert(rows == LCD_HEIGHT && page == 0);
    /* Disjoint rectangles must survive alternating pages, including padding. */
    for (int i = 0; i < 100; i++) {
        int x = i * 73 % (LCD_WIDTH - 9), y = i * 47 % (LCD_HEIGHT - 5);
        for (int dy = 0; dy < 5; dy++)
            for (int dx = 0; dx < 9; dx++) frame[y+dy][x+dx] += i + 1;
        lcd_update_rect(x,y,9,5); verify();
    }
    /* Deferred repair must not expose edits outside the requested rectangle. */
    frame[1][1]++; lcd_update_rect(1,1,1,1); verify();
    uint32_t saved = frame[1][1]; frame[1][1]++;
    frame[10][10]++; lcd_update_rect(10,10,1,1);
    frame[1][1] = saved; verify();
    unsigned before = pans;
    lcd_update_rect(-20,0,10,5); lcd_update_rect(LCD_WIDTH,0,10,5);
    assert(pans == before);
    int old = page; unsigned waited = waits;
    frame[2][3]++; fail_pan = true; lcd_update_rect(3,2,1,1);
    assert(page == old && var.yoffset == (unsigned)page * var.yres && waits == waited);
    fail_pan = false; lcd_update_rect(9,9,1,1); verify();
    lcd_update_rect(0,0,1,1); verify();
    active = false; before = pans; lcd_update(); assert(pans == before);
    lcd_enable(true); verify();
    pages = 1; page = 0; synced = false; lcd_update(); verify();
    for (int p = 0; p < 2; p++)
        for (int x = 0; x < LCD_WIDTH; x++)
            for (unsigned y = LCD_HEIGHT; y < stride; y++)
                assert(panel[p * LCD_WIDTH * stride + x * stride + y] == 0);
    free(panel);
    return 0;
}
'''
    power = r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#define HZ 100
#define POWER_INPUT_USB_CHARGER 1
#define POWER_INPUT_NONE 0
static long current_tick;
static unsigned reads;
static bool read_ok = true;
static const char *status = "Charging";
static int capacity = 75;
#include "firmware/target/hosted/shanling/power-q2.c"
bool sysfs_get_string(const char *p, char *out, int n) {
    (void)p; reads++; strncpy(out, status, n); return read_ok;
}
bool sysfs_get_int(const char *p, int *v) { (void)p; *v = capacity; return read_ok; }
int main(void) {
    read_ok = false; assert(_battery_level() == -1);
    read_ok = true; assert(_battery_level() == 75);
    read_ok = false; capacity = -1; assert(_battery_level() == 75);
    read_ok = true; capacity = 101; assert(_battery_level() == 75);
    capacity = 0; assert(_battery_level() == 0);
    assert(charging_state() && power_input_status() == POWER_INPUT_USB_CHARGER);
    assert(reads == 1); /* startup samples immediately; callers share the result */
    for (current_tick = 1; current_tick < HZ; current_tick++) charging_state();
    assert(reads == 1);
    status = "Full"; assert(!charging_state());
    assert(power_input_status() == POWER_INPUT_USB_CHARGER && reads == 2);
    current_tick += HZ; read_ok = false; status = "";
    assert(power_input_status() == POWER_INPUT_USB_CHARGER && !charging_state());
    current_tick += HZ; read_ok = true; status = "Unknown";
    assert(power_input_status() == POWER_INPUT_USB_CHARGER);
    current_tick += HZ; status = "Discharging";
    assert(power_input_status() == POWER_INPUT_NONE && !charging_state());
    current_tick = LONG_MAX - HZ / 2; status = "Charging";
    assert(charging_state());
    current_tick = LONG_MIN + HZ / 2; status = "Full";
    assert(!charging_state() && power_input_status() == POWER_INPUT_USB_CHARGER);
    return 0;
}
'''
    for name, source in (('display', display), ('power', power)):
        src, exe = tmp / (name + '.c'), tmp / name
        src.write_text(source)
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined',
                        '-I', str(tmp), '-I', str(root), '-I',
                        str(root / 'firmware/target/hosted'), str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
print('Q2 rotation/pages/pan failure/clipping/wake, battery errors/charger cache/full: OK')
