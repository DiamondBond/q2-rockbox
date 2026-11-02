#!/usr/bin/env python3
"""Host check: python3 tools/shanlingq2/test_dac_idle.py (requires cc and ALSA headers)."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]


def function(name):
    source = (root / "firmware/target/hosted/pcm-alsa.c").read_text()
    start = source.index("static void " + name + "(")
    return source[start:source.index("\n}\n", start)]


# The wake is sink_dma_start's first statement, the stamp sink_dma_stop's last
assert "{\n#ifdef HAVE_SHANLINGQ2_CODEC\n    shanlingq2_dac_wake();\n#endif\n" in function("sink_dma_start")
assert function("sink_dma_stop").endswith("#ifdef HAVE_SHANLINGQ2_CODEC\n    shanlingq2_dac_idle();\n#endif")

with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    (tmp / "config.h").write_text("")
    (tmp / "audiohw.h").write_text("""
#include <stdbool.h>
#define SOUND_HIGH_POWER 0
void audiohw_mute(int);
""")
    (tmp / "button.h").write_text("bool headphones_inserted(void);\n")
    (tmp / "kernel.h").write_text("""
#define HZ 100
#define TIME_BEFORE(a, b) ((long)(a) - (long)(b) < 0)
extern volatile long current_tick;
int tick_add_task(void (*)(void));
""")
    (tmp / "pcm-alsa.h").write_text("""
void pcm_alsa_set_playback_device(const char *);
void pcm_set_mixer_volume(int, int);
""")
    (tmp / "test.c").write_text(r'''
#include <assert.h>
#include <stdarg.h>
#include <string.h>
#define open mock_open
#define close mock_close
#define ioctl mock_ioctl
#include "firmware/target/hosted/shanling/shanlingq2_codec.c"

volatile long current_tick;
static void (*task)(void);
static int tasks, bluetooth, phones, opens, mixer_calls, calls;
static struct { unsigned long req; int value; } log_[32];

int tick_add_task(void (*f)(void)) { task = f; tasks++; return 0; }
int mock_open(const char *path, int flags, ...)
{
    (void)flags;
    assert(!strcmp(path, "/dev/shanling_dac"));
    opens++;
    return 42;
}
int mock_close(int fd) { assert(fd == 42); return 0; }
int mock_ioctl(int fd, unsigned long req, ...)
{
    assert(fd == 42 && calls < 32);
    va_list args;
    va_start(args, req);
    log_[calls].req = req;
    log_[calls++].value = *va_arg(args, int *);
    va_end(args);
    return 0;
}
int snd_pcm_open(snd_pcm_t **pcm, const char *name,
                 snd_pcm_stream_t stream, int mode)
{
    *pcm = NULL; (void)name; (void)stream; (void)mode;
    return bluetooth ? 0 : -1;
}
int snd_pcm_close(snd_pcm_t *pcm) { (void)pcm; return 0; }
bool headphones_inserted(void) { return phones; }
void pcm_alsa_set_playback_device(const char *name) { (void)name; }
void pcm_set_mixer_volume(int left, int right)
{
    (void)left; (void)right;
    mixer_calls++;
}

/* Run the tick tasks up to and including this many ticks from now */
static void ticks(long n)
{
    while (n-- > 0) {
        current_tick++;
        if (task)
            task();
    }
}
static void no_calls(void) { assert(calls == 0); }
static void expect_replay(int g, int f, int m)
{
    assert(calls == 7);
    assert(log_[0].req == DAC_HEADSET && log_[0].value == 1);
    assert(log_[1].req == DAC_POWER && log_[1].value == 1);
    assert(log_[2].req == DAC_PCM && log_[2].value == 0);
    assert(log_[3].req == DAC_VOLUME && log_[3].value == (100 << 8 | 100));
    assert(log_[4].req == DAC_GAIN && log_[4].value == g);
    assert(log_[5].req == DAC_FILTER && log_[5].value == f);
    assert(log_[6].req == DAC_MUTE && log_[6].value == m);
    calls = 0;
}
static void expect_off(void)
{
    assert(calls == 2);
    assert(log_[0].req == DAC_MUTE && log_[0].value == 1);
    assert(log_[1].req == DAC_POWER && log_[1].value == 0);
    calls = 0;
}
/* From a stop just now: nothing until the 30 s are up, then off, then quiet */
static void expires_at_30s(void)
{
    ticks(30 * HZ - 1);
    no_calls();
    ticks(1);
    expect_off();
    ticks(60 * HZ);
    no_calls();
}

static void *tick_thread(void *arg)
{
    (void)arg;
    task();
    return NULL;
}

int main(void)
{
    alarm(20); /* a lock taken twice dies here rather than hangs */

    /* Preinit doesn't arm, postinit does: browsing with nothing played */
    audiohw_preinit();
    expect_replay(0, 0, 0);
    assert(!tasks);
    ticks(60 * HZ);
    no_calls();
    audiohw_set_volume(-200, -300);
    audiohw_postinit();
    assert(tasks == 1);
    int mixed = mixer_calls;
    expires_at_30s();
    /* the expiry leaves the stored mute, the volume and the mixer alone */
    assert(muted == 0 && vol[0] == -200 && vol[1] == -300 && mixer_calls == mixed);

    /* Changes while off: stored, no ioctl, and in the one replay */
    audiohw_set_power_mode(SOUND_HIGH_POWER);
    audiohw_set_filter_roll_off(3);
    audiohw_mute(true);
    no_calls();
    assert(mixer_calls == mixed + 1); /* Rockbox's mixer still mutes */
    shanlingq2_dac_wake();
    expect_replay(1, 3, 1);
    shanlingq2_dac_wake();
    no_calls();

    /* Playing never expires; while on, changes reach the DAC */
    ticks(120 * HZ);
    no_calls();
    audiohw_mute(false);
    assert(calls == 1 && log_[0].req == DAC_MUTE && log_[0].value == 0);
    calls = 0;
    audiohw_set_filter_roll_off(2);
    assert(calls == 1 && log_[0].req == DAC_FILTER && log_[0].value == 2);
    calls = 0;
    audiohw_set_power_mode(1);
    assert(calls == 1 && log_[0].req == DAC_GAIN && log_[0].value == 0);
    calls = 0;

    /* The stamp alone in the idle hook; a start before expiry cancels it */
    shanlingq2_dac_idle();
    no_calls();
    ticks(30 * HZ - 1);
    no_calls();
    shanlingq2_dac_wake();
    no_calls();
    ticks(120 * HZ);
    no_calls();

    /* A later stop restarts the 30 s, whatever came before */
    shanlingq2_dac_idle();
    ticks(10 * HZ);
    shanlingq2_dac_idle();
    expires_at_30s();
    /* Replays the mute as stored: unmuted here */
    shanlingq2_dac_wake();
    expect_replay(0, 2, 0);

    /* A stop at tick 0 still counts, about the same 30 s */
    current_tick = -1;
    ticks(1);
    assert(current_tick == 0);
    shanlingq2_dac_idle();
    assert(idle_since);
    ticks(30 * HZ);
    no_calls();
    ticks(1);
    expect_off();
    shanlingq2_dac_wake();
    expect_replay(0, 2, 0);

    /* What SIGIO reaches takes no lock, nor does an unexpired tick: these
     * would deadlock under dac_mtx */
    pthread_mutex_lock(&dac_mtx);
    shanlingq2_dac_idle();
    audiohw_mute(true);
    assert(calls == 1 && log_[0].req == DAC_MUTE && log_[0].value == 1);
    calls = 0;
    audiohw_mute(false);
    calls = 0;
    ticks(10);
    no_calls();
    pthread_mutex_unlock(&dac_mtx);

    /* A tick past its compare, waiting on the lock when a wake clears the
     * stamp, must look again: no power off under a start */
    ticks(30 * HZ);
    expect_off();
    shanlingq2_dac_wake();
    expect_replay(0, 2, 0);
    shanlingq2_dac_idle();
    current_tick += 30 * HZ;
    pthread_t thread;
    pthread_mutex_lock(&dac_mtx);
    assert(!pthread_create(&thread, NULL, tick_thread, NULL));
    usleep(50000);
    idle_since = 0;
    pthread_mutex_unlock(&dac_mtx);
    assert(!pthread_join(thread, NULL));
    no_calls();

    /* Fallback during playback: the stamp stays clear, no power off */
    audiohw_close();
    calls = 0;
    bluetooth = 1;
    audiohw_preinit();
    no_calls();
    shanlingq2_dac_wake();
    no_calls();
    bluetooth = 0;
    audiohw_preinit();
    expect_replay(0, 2, 0);
    ticks(120 * HZ);
    no_calls();
    shanlingq2_dac_idle();
    expires_at_30s();
    shanlingq2_dac_wake();
    expect_replay(0, 2, 0);

    /* Bluetooth, the DAC never opened: nothing at all reaches it */
    audiohw_close();
    calls = 0;
    bluetooth = 1;
    int opened = opens;
    audiohw_preinit();
    shanlingq2_dac_idle();
    ticks(60 * HZ);
    shanlingq2_dac_wake();
    audiohw_mute(false);
    audiohw_set_power_mode(0);
    audiohw_set_filter_roll_off(1);
    ticks(60 * HZ);
    shanlingq2_dac_idle();
    audiohw_mute(true);
    ticks(60 * HZ);
    audiohw_close();
    no_calls();
    assert(opens == opened);

    /* Fallback while idle on Bluetooth: on, then off on the stamp it had */
    audiohw_preinit();
    shanlingq2_dac_idle();
    ticks(60 * HZ);
    bluetooth = 0;
    audiohw_preinit();
    expect_replay(1, 1, 0);
    ticks(1);
    expect_off();
    shanlingq2_dac_wake();
    expect_replay(1, 1, 0);

    /* The DAC left on when a headset took over: off on idle, and a start stays
     * off while Bluetooth is the output, until it falls back */
    bluetooth = 1;
    audiohw_preinit();
    no_calls();
    shanlingq2_dac_idle();
    expires_at_30s();
    shanlingq2_dac_wake();
    no_calls();
    bluetooth = 0;
    audiohw_preinit();
    expect_replay(1, 1, 0);

    /* Headphones win over a headset that would open: the DAC again. Close left
     * it muted, so preinit replays that and then unmutes. */
    audiohw_close();
    calls = 0;
    bluetooth = phones = 1;
    audiohw_preinit();
    assert(!shanlingq2_bt_output());
    assert(calls == 8 && log_[7].req == DAC_MUTE && log_[7].value == 0);
    calls = 7;
    expect_replay(1, 1, 1);

    /* After close the tick and a stray wake stay harmless */
    audiohw_close();
    calls = 0;
    assert(!dac_on);
    shanlingq2_dac_idle();
    ticks(60 * HZ);
    shanlingq2_dac_wake();
    assert(!dac_on);
    audiohw_set_filter_roll_off(0);
    audiohw_set_power_mode(1);
    ticks(60 * HZ);
    no_calls();
    assert(tasks == 1); /* the one postinit's */
    return 0;
}
''')
    subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                    "-pthread", f"-I{tmp}", f"-I{root}", str(tmp / "test.c"),
                    "-o", str(tmp / "test")], check=True)
    subprocess.run([str(tmp / "test")], check=True, timeout=30)
print("Q2 DAC idle checks passed")
