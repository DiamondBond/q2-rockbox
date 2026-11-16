#!/usr/bin/env python3
"""Host check for the Q2's side keys in keymap-ipod.c (requires cc).

A tap of Previous or Next skips a track from screens that don't use the key:
its release reaches button_context_standard's BUTTON_MULTIMEDIA_PREV/NEXT,
which default_event_handler takes. A screen whose own context acts on the
press must not also skip on the release that falls through to it. Every
context goes through the real keymap and apps/action.c's own lookup."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
action_c = (root / 'apps/action.c').read_text()
action_h = (root / 'apps/action.h').read_text()
button_h = (root / 'firmware/export/button.h').read_text()
keymap = (root / 'apps/keymaps/keymap-ipod.c').read_text()


def function(name):
    start = re.search(r'static inline [^\n]+\b' + name + r'\(', action_c).start()
    return action_c[start:action_c.index('\n}\n', start) + 3]


# BUTTON_REL, BUTTON_REPEAT and the multimedia keys, one-line defines only
buttons = '\n'.join(m.group(0) for m in re.finditer(
    r'^#define BUTTON_(?:NONE|REL|REPEAT|MULTIMEDIA\w*)\s+[^\\\n]+$', button_h, re.M))
# the CONTEXT_ and ACTION_ values and struct button_mapping
actions = action_h[action_h.index('#define TIMEOUT_BLOCK'):]
actions = actions[:actions.index('};', actions.index('struct button_mapping {')) + 2]
# action_cur_t and action_last_t
types = action_c[action_c.index('typedef struct'):]
types = types[:types.index('} action_last_t;') + len('} action_last_t;')]

source = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define SHANLING_Q2
#define HAVE_BACKLIGHT
#include "firmware/target/hosted/shanling/button-target.h"
#define BUTTON_REMOTE 0
''' + buttons + '\n' + actions + r'''
static struct { bool hold_lr_for_scroll_in_list; } global_settings;
''' + re.sub(r'^#include .*$', '', keymap, flags=re.M) + r'''
bool is_keys_locked(void) { return false; }
static void *core_get_data(int handle) { (void)handle; return NULL; }
''' + types + '\n' + function('has_flag') + function('action_code_worker') + \
    function('get_next_context') + function('action_code_lookup') + r'''
static int lookup(int context, int button, int last_button)
{
    action_last_t last;
    action_cur_t cur;
    memset(&last, 0, sizeof last);
    memset(&cur, 0, sizeof cur);
    last.button = last_button;
    cur.button = button;
    cur.context = context;
    action_code_lookup(&last, &cur);
    return cur.action;
}

static int failures;

static void expect(const char *what, int context, int button, int last_button,
                   int want)
{
    int got = lookup(context, button, last_button);
    if (got != want) {
        printf("context %#x, %s: got %#x, want %#x\n", (unsigned)context, what,
               (unsigned)got, (unsigned)want);
        failures++;
    }
}

static bool acts(int action)
{
    return action != ACTION_NONE && action != ACTION_UNKNOWN;
}

int main(void)
{
    static const int keys[] = { BUTTON_LEFT, BUTTON_RIGHT };
    static const char *const names[] = { "Previous", "Next" };
    static const int skip[] = { BUTTON_MULTIMEDIA_PREV, BUTTON_MULTIMEDIA_NEXT };

    for (int hold_lr = 0; hold_lr < 2; hold_lr++) {
        global_settings.hold_lr_for_scroll_in_list = hold_lr;

        /* A press that does something here: its release must not skip too */
        for (int c = 0; c <= LAST_CONTEXT_PLACEHOLDER; c++) {
            int context = c < LAST_CONTEXT_PLACEHOLDER ? c : (int)(CONTEXT_CUSTOM|CONTEXT_TREE);
            for (int k = 0; k < 2; k++) {
                int press = lookup(context, keys[k], BUTTON_NONE);
                int tap = lookup(context, keys[k]|BUTTON_REL, keys[k]);
                int held = lookup(context, keys[k]|BUTTON_REL, keys[k]|BUTTON_REPEAT);
                if (acts(press) && (tap & BUTTON_MULTIMEDIA)) {
                    printf("context %#x: a %s tap acts (%#x) and skips (%#x)\n",
                           (unsigned)context, names[k], (unsigned)press, (unsigned)tap);
                    failures++;
                }
                if (held & BUTTON_MULTIMEDIA) {
                    printf("context %#x: releasing a held %s skips\n",
                           (unsigned)context, names[k]);
                    failures++;
                }
            }
        }

        /* Lists, the file browser and the main menu: a tap skips */
        static const int lists[] = { CONTEXT_STD, CONTEXT_LIST, CONTEXT_TREE,
                                     CONTEXT_MAINMENU, CONTEXT_CUSTOM|CONTEXT_TREE };
        for (unsigned i = 0; i < sizeof lists / sizeof *lists; i++) {
            for (int k = 0; k < 2; k++)
                expect("side key tap", lists[i], keys[k]|BUTTON_REL, keys[k], skip[k]);
            /* Return is the only back key; the centre selects */
            expect("Return", lists[i], BUTTON_MENU|BUTTON_REL, BUTTON_MENU, ACTION_STD_CANCEL);
            expect("centre", lists[i], BUTTON_SELECT|BUTTON_REL, BUTTON_SELECT, ACTION_STD_OK);
        }
    }

    /* "Hold left/right for scrolling in lists": a hold pages */
    global_settings.hold_lr_for_scroll_in_list = true;
    expect("held Previous", CONTEXT_TREE, BUTTON_LEFT|BUTTON_REPEAT, BUTTON_LEFT, ACTION_TREE_PGLEFT);
    expect("held Next", CONTEXT_TREE, BUTTON_RIGHT|BUTTON_REPEAT, BUTTON_RIGHT, ACTION_TREE_PGRIGHT);
    global_settings.hold_lr_for_scroll_in_list = false;

    /* The WPS skips through its own actions */
    expect("WPS Previous", CONTEXT_WPS, BUTTON_LEFT|BUTTON_REL, BUTTON_LEFT, ACTION_WPS_SKIPPREV);
    expect("WPS Next", CONTEXT_WPS, BUTTON_RIGHT|BUTTON_REL, BUTTON_RIGHT, ACTION_WPS_SKIPNEXT);

    /* Settings (and the EQ, colour and time screens): the press changes the
     * value, the release does nothing */
    static const int settings[] = { CONTEXT_SETTINGS, CONTEXT_SETTINGS_EQ,
                                    CONTEXT_SETTINGS_COLOURCHOOSER, CONTEXT_SETTINGS_TIME,
                                    CONTEXT_SETTINGS_RECTRIGGER };
    for (unsigned i = 0; i < sizeof settings / sizeof *settings; i++) {
        expect("settings Previous", settings[i], BUTTON_LEFT, BUTTON_NONE, ACTION_STD_PREV);
        expect("settings Next", settings[i], BUTTON_RIGHT, BUTTON_NONE, ACTION_STD_NEXT);
        for (int k = 0; k < 2; k++)
            expect("settings tap release", settings[i], keys[k]|BUTTON_REL, keys[k], ACTION_NONE);
    }

    /* Quickscreen: the press changes that side's setting, the release nothing */
    expect("quickscreen Previous", CONTEXT_QUICKSCREEN, BUTTON_LEFT, BUTTON_NONE, ACTION_QS_LEFT);
    expect("quickscreen Next", CONTEXT_QUICKSCREEN, BUTTON_RIGHT, BUTTON_NONE, ACTION_QS_RIGHT);
    for (int k = 0; k < 2; k++)
        expect("quickscreen tap release", CONTEXT_QUICKSCREEN, keys[k]|BUTTON_REL, keys[k], ACTION_NONE);

    /* Yes/no: any other action answers no, so a side key neither skips nor
     * answers; the centre is yes, Play and Return no */
    for (int k = 0; k < 2; k++)
        expect("yes/no tap", CONTEXT_YESNOSCREEN, keys[k]|BUTTON_REL, keys[k], ACTION_NONE);
    expect("yes/no centre", CONTEXT_YESNOSCREEN, BUTTON_SELECT, BUTTON_NONE, ACTION_YESNO_ACCEPT);
    expect("yes/no Play", CONTEXT_YESNOSCREEN, BUTTON_PLAY, BUTTON_NONE, ACTION_STD_CANCEL);
    expect("yes/no Return", CONTEXT_YESNOSCREEN, BUTTON_MENU|BUTTON_REL, BUTTON_MENU, ACTION_STD_CANCEL);

    return failures != 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    (tmp / 'test.c').write_text(source)
    # -Wimplicit-fallthrough=0 as configure sets it: get_context_mapping
    # falls through on purpose
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wimplicit-fallthrough=0', f'-I{root}',
                    str(tmp / 'test.c'), '-o', str(tmp / 'test')], check=True)
    subprocess.run([str(tmp / 'test')], check=True)
print('Q2 side keys: OK')
