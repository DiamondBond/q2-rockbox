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
#include <pthread.h>
#define LCD_WIDTH 375
#define LCD_HEIGHT 320
#define LCD_EVENT_ACTIVATION 1
typedef uint32_t fb_data;
static fb_data frame[LCD_HEIGHT][LCD_WIDTH];
static unsigned rows, pans, waits, blanks, sleeps;
static pthread_t main_thread;
static bool active = true, fail_pan;
static const fb_data *address(int x, int y) { rows++; return &frame[y][x]; }
#define FBADDR(x,y) address(x,y)
static bool lcd_active(void) { return active; }
void lcd_set_active(bool on) { active = on; }
static void send_event(int id, void *p) { (void)id; (void)p; }
static void panicf(const char *s, ...) { (void)s; abort(); }
#define ioctl mock_ioctl
#define sleep mock_sleep
#include "firmware/target/hosted/shanling/lcd-q2.c"
unsigned mock_sleep(unsigned ticks) { assert(ticks == 1); sleeps++; usleep(1000); return 0; }
int mock_ioctl(int dev, unsigned long request, ...) {
    assert(dev == 42);
    if (request == FBIOPAN_DISPLAY) { pans++; return fail_pan ? -1 : 0; }
    if (request == FBIO_WAITFORVSYNC) waits++;
    if (request == FBIOBLANK) { /* off the green threads' OS thread, nothing panning */
        assert(!pthread_equal(pthread_self(), main_thread) && !active);
        usleep(20000); blanks++;
    }
    return 0;
}
static void verify(void) {
    for (int y = 0; y < LCD_HEIGHT; y++)
        for (int x = 0; x < LCD_WIDTH; x++)
            assert(panel[page * LCD_WIDTH * stride + x * stride + LCD_HEIGHT - 1 - y]
                   == (frame[y][x] | 0xff000000));
}
int main(void) {
    main_thread = pthread_self();
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
    lcd_enable(true); verify(); assert(active && blanks == 1 && sleeps > 0);
    pages = 1; page = 0; synced = false; lcd_update(); verify();
    for (int p = 0; p < 2; p++)
        for (int x = 0; x < LCD_WIDTH; x++)
            for (unsigned y = LCD_HEIGHT; y < stride; y++)
                assert(panel[p * LCD_WIDTH * stride + x * stride + y] == 0);
    lcd_enable(false); assert(!active && blanks == 2);
    free(panel);
    return 0;
}
'''
    power = r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#define HZ 100
#define POWER_INPUT_USB_CHARGER 1
#define POWER_INPUT_NONE 0
#define TIME_AFTER(a,b) ((long)(b) - (long)(a) < 0)
#define TIME_BEFORE(a,b) TIME_AFTER(b,a)
static long current_tick;
static unsigned reads, gauge_reads, jack_reads, jack_setups;
static bool read_ok = true;
static const char *status = "Charging";
static int capacity = 75;
static const char *jack = "";
static int mock_open(const char *p, int flags) { (void)p; (void)flags; return 7; }
static int mock_ioctl(int fd, unsigned long request, const void *arg);
#define open mock_open
#define ioctl mock_ioctl
#include "firmware/target/hosted/shanling/power-q2.c"
static int mock_ioctl(int fd, unsigned long request, const void *arg) {
    assert(fd == 7);
    if (request == GPIO_SET_FUNC) { /* as stock's toolsGpioInput(pin, 0), before any read */
        const struct gpio_func *f = arg;
        assert(jack_reads == 0 && !strcmp(f->pin, jack_setups ? "PA08" : "PA07") && f->count == 2
               && !strcmp(f->args[0], "input") && !strcmp(f->args[1], "pull_hiz"));
        jack_setups++;
        return 0;
    }
    assert(request == GPIO_GET_VALUE && jack_setups == 2);
    jack_reads++;
    return strcmp(arg, jack) != 0; /* active low */
}
bool sysfs_get_string(const char *p, char *out, int n) {
    (void)p; reads++; strncpy(out, status, n); return read_ok;
}
bool sysfs_get_int(const char *p, int *v) {
    (void)p; gauge_reads++; *v = capacity; return read_ok;
}
int main(void) {
    /* The jacks: the first call configures both pins, then reads both, at tick 0
     * too, then once per HZ/4. */
    jack = "PA08"; assert(headphones_inserted() && jack_reads == 2 && jack_setups == 2);
    jack = "";
    for (; current_tick < HZ / 4; current_tick++) assert(headphones_inserted());
    assert(jack_reads == 2);
    assert(!headphones_inserted() && jack_reads == 4);
    jack = "PA07"; current_tick += HZ / 4 - 1;
    assert(!headphones_inserted() && jack_reads == 4);
    current_tick++; assert(headphones_inserted() && jack_reads == 6);
    jack = ""; current_tick = LONG_MAX - 5; assert(!headphones_inserted());
    jack = "PA07"; current_tick = LONG_MAX; assert(!headphones_inserted());
    current_tick = LONG_MIN + HZ / 4 - 7; assert(!headphones_inserted());
    current_tick++; assert(headphones_inserted());
    /* The gauge: each call tries until a valid reading, then once per 10 s,
     * keeping the last valid one through errors. */
    current_tick = 0;
    read_ok = false; assert(_battery_level() == -1);
    read_ok = true; capacity = 101; assert(_battery_level() == -1 && gauge_reads == 2);
    capacity = 75; assert(_battery_level() == 75 && gauge_reads == 3);
    capacity = 50;
    for (; current_tick < 10 * HZ; current_tick++) assert(_battery_level() == 75);
    assert(gauge_reads == 3);
    read_ok = false; capacity = -1; assert(_battery_level() == 75 && gauge_reads == 4);
    assert(_battery_level() == 75 && gauge_reads == 4);
    current_tick += 10 * HZ; read_ok = true; capacity = 101;
    assert(_battery_level() == 75 && gauge_reads == 5);
    current_tick += 10 * HZ; capacity = 0; assert(_battery_level() == 0 && gauge_reads == 6);
    current_tick = LONG_MAX - HZ; capacity = 20; assert(_battery_level() == 20);
    capacity = 30; current_tick = LONG_MAX; assert(_battery_level() == 20);
    current_tick = LONG_MIN + 9 * HZ - 2; assert(_battery_level() == 20);
    current_tick++; assert(_battery_level() == 30);
    current_tick = 0;
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
        # -fno-strict-overflow as the firmware: TIME_AFTER relies on it at wrap
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined', '-pthread',
                        '-D_GNU_SOURCE', # as the Q2 build: pthread_tryjoin_np
                        '-fno-strict-overflow', '-I', str(tmp), '-I', str(root), '-I',
                        str(root / 'firmware/target/hosted'), str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
print('Q2 rotation/pages/pan failure/clipping/wake/blank off-thread, battery errors/charger, jack and gauge caches/full: OK')
