#!/usr/bin/env python3
"""Native checks for button-q2.c's device matching and headset keys."""
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

keymap = (root / 'apps/keymaps/keymap-ipod.c').read_text()
q2 = keymap[keymap.index('#ifdef SHANLING_Q2'):keymap.index('#else')]
assert 'BUTTON_MULTIMEDIA_PREV,   BUTTON_LEFT|BUTTON_REL' in q2
assert 'BUTTON_MULTIMEDIA_NEXT,   BUTTON_RIGHT|BUTTON_REL' in q2
assert 'ACTION_STD_CANCEL,        BUTTON_MENU|BUTTON_REL' in q2

with tempfile.TemporaryDirectory() as tmp:
    c, exe = Path(tmp, 't.c'), Path(tmp, 't')
    c.write_text(source)
    subprocess.run(['cc', '-Wall', '-Werror', '-o', exe, c], check=True)
    subprocess.run([exe], check=True)
print('ok')
