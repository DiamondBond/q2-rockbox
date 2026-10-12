#!/usr/bin/env python3
"""Host regression check for Doom's Q2 menu chord (requires cc)."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'apps/plugins/doom/i_video.c').read_text()
# Exercise the actual key mappings and polling code without the renderer.
source = source[source.index('#if (CONFIG_KEYPAD == IPOD_4G_PAD)'):source.index('void I_StartTic')]
keys = (root / 'apps/plugins/doom/doomdef.h').read_text()
keys = '\n'.join(line for line in keys.splitlines() if line.startswith('#define KEY_'))
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include "firmware/target/hosted/shanling/button-target.h"
#define SHANLING_Q2
#define CONFIG_KEYPAD 1
#define IPOD_4G_PAD 1
#define BUTTON_NONE 0
#define BUTTON_REPEAT 0x04000000
#define IDATA_ATTR
#include "keys.h"
enum { ev_keydown, ev_keyup, ev_scroll };
typedef struct { int type, data1; } event_t;
static event_t events[32];
static unsigned state;
static int count;
static void D_PostEvent(event_t *event) {
    assert(count < 32);
    events[count++] = *event;
}
static int button_get(bool block) { (void)block; return 0; }
static int button_get_w_tmo(int timeout) { (void)timeout; return 0; }
static unsigned button_status(void) { return state; }
static struct {
    int (*button_get)(bool);
    int (*button_get_w_tmo)(int);
    unsigned (*button_status)(void);
} api = { button_get, button_get_w_tmo, button_status }, *rb = &api;
#include "input.c"
static void poll(unsigned buttons) { count = 0; state = buttons; getkey(); }
static void expect(int index, int type, int key) {
    assert(index < count);
    assert(events[index].type == type && events[index].data1 == key);
}
int main(void) {
    assert(!(DOOMBUTTON_ESC & BUTTON_MAIN));
    poll(BUTTON_LEFT);
    assert(count == 1); expect(0, ev_keydown, KEY_LEFTARROW);
    poll(BUTTON_LEFT | BUTTON_RIGHT);
    assert(count == 2);
    expect(0, ev_keyup, KEY_LEFTARROW);
    expect(1, ev_keydown, KEY_ESCAPE);
    poll(BUTTON_LEFT | BUTTON_RIGHT); assert(count == 0);
    poll(BUTTON_RIGHT);
    assert(count == 2);
    expect(0, ev_keyup, KEY_ESCAPE);
    expect(1, ev_keydown, KEY_RIGHTARROW);
    poll(0);
    assert(count == 1); expect(0, ev_keyup, KEY_RIGHTARROW);
    poll(BUTTON_LEFT | BUTTON_RIGHT);
    assert(count == 1); expect(0, ev_keydown, KEY_ESCAPE);
    poll(0);
    assert(count == 1); expect(0, ev_keyup, KEY_ESCAPE);
    poll(BUTTON_SELECT);
    expect(0, ev_keydown, KEY_ENTER);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    tmp = Path(directory)
    (tmp / 'input.c').write_text(source)
    (tmp / 'keys.h').write_text(keys)
    (tmp / 'test.c').write_text(harness)
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root), str(tmp / 'test.c'), '-o', str(tmp / 'test')], check=True)
    subprocess.run([str(tmp / 'test')], check=True)
print('Doom Q2 menu chord: passed')
