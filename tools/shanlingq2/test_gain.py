#!/usr/bin/env python3
"""Host regression check: python3 tools/shanlingq2/test_gain.py (requires cc)."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]


def function(path, name):
    source = (root / path).read_text()
    start = source.index("static void " + name + "(")
    return source[start:source.index("\n}\n", start) + 2]


source = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
typedef int32_t sample_t;
#define PCM_DC_OFFSET_VALUE 0
static int32_t dig_vol_mult_l, dig_vol_mult_r;
static int32_t dig_vol_current_l, dig_vol_current_r;
struct mixer_channel {
    const void *start;
    uint32_t amplitude, current_amplitude;
};
static int16_t clip_sample_16(int32_t s) {
    return s > 32767 ? 32767 : s < -32768 ? -32768 : s;
}
'''
source += function("firmware/target/hosted/pcm-alsa.c", "scale_frames")
source += function("firmware/pcm_mixer.c", "mixer_write_ramp")
source += r'''
int main(void) {
    int16_t input[512], output[512];
    int32_t wide[512];
    for (int i = 0; i < 512; i++) input[i] = 16000;
    struct mixer_channel chan = { input, 0, 65536 };
    int previous = 16000;
    mixer_write_ramp(output, &chan, sizeof(input), false);
    for (int i = 0; i < 512; i += 2) {
        assert(output[i] == output[i+1]);
        assert(output[i] <= previous && previous - output[i] <= 64);
        previous = output[i];
    }
    assert(previous == 0 && chan.current_amplitude == 0);
    chan.amplitude = 65536;
    mixer_write_ramp(output, &chan, sizeof(input), false);
    assert(output[0] < 64 && output[510] == 16000);
    mixer_write_ramp(output, &chan, sizeof(input), true);
    assert(output[510] == 32000);
    mixer_write_ramp(output, &chan, sizeof(input), true);
    assert(output[510] == 32767);
    for (int target = 65536; target >= 0; target -= 16384) {
        dig_vol_mult_l = dig_vol_mult_r = target;
        int32_t before = dig_vol_current_l * 16000;
        scale_frames(wide, input, 256);
        for (int i = 0; i < 512; i += 2) {
            assert(wide[i] == wide[i+1]);
            assert(llabs((long long)wide[i] - before) <= 16000 * 256);
            before = wide[i];
        }
        assert(dig_vol_current_l == target && wide[510] == target * 16000);
    }
    dig_vol_mult_l = 65536;
    dig_vol_mult_r = 32768;
    scale_frames(wide, input, 1);
    assert(wide[0] == 16000 * 65536 && wide[1] == 16000 * 32768);
    scale_frames(wide, input, 0);
    input[0] = -32768; input[1] = 32767;
    scale_frames(wide, input, 1);
    assert(wide[0] == INT32_MIN && wide[1] == 32767 * 32768);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / "gain.c"
    exe = Path(tmp) / "gain"
    src.write_text(source)
    subprocess.run(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=undefined", str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("gain ramps: OK")
