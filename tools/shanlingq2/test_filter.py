#!/usr/bin/env python3
"""Host check: python3 tools/shanlingq2/test_filter.py (requires cc and ALSA headers)."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    (tmp / "config.h").write_text("")
    (tmp / "audiohw.h").write_text("""
#include <stdbool.h>
#define SOUND_HIGH_POWER 0
void audiohw_mute(int);
""")
    (tmp / "button.h").write_text("bool headphones_inserted(void);\n")
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

static int bluetooth, phones, last_filter = -1, writes;
int mock_open(const char *path, int flags, ...)
{
    (void)flags;
    assert(!strcmp(path, "/dev/shanling_dac"));
    return 42;
}
int mock_close(int fd) { assert(fd == 42); return 0; }
int mock_ioctl(int fd, unsigned long req, ...)
{
    assert(fd == 42);
    va_list args;
    va_start(args, req);
    int value = *va_arg(args, int *);
    va_end(args);
    if (req == 0xc0044d19) { last_filter = value; writes++; }
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
void pcm_set_mixer_volume(int left, int right) { (void)left; (void)right; }

int main(void)
{
    audiohw_preinit();
    assert(last_filter == 0 && writes == 1);
    for (int i = 0; i < 4; i++) {
        audiohw_set_filter_roll_off(i);
        assert(last_filter == i && writes == i + 2);
    }
    audiohw_set_filter_roll_off(-1);
    audiohw_set_filter_roll_off(4);
    assert(last_filter == 3 && writes == 5);
    audiohw_close();
    bluetooth = 1;
    audiohw_preinit();
    audiohw_set_filter_roll_off(2);
    assert(writes == 5);
    bluetooth = 0;
    audiohw_preinit();
    assert(last_filter == 2 && writes == 6);
    audiohw_close();
    audiohw_preinit();
    assert(last_filter == 2 && writes == 7);
    audiohw_close();
    /* headphones in a jack: the DAC, though Bluetooth would open */
    bluetooth = phones = 1;
    audiohw_preinit();
    assert(writes == 8);
    audiohw_close();
}
''')
    subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", f"-I{tmp}",
                    f"-I{root}", str(tmp / "test.c"), "-o", str(tmp / "test")],
                   check=True)
    subprocess.run([str(tmp / "test")], check=True)
print("Q2 DAC filter checks passed")
