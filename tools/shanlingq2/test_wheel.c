/* Host check of the Q2 wheel's tick logic: cc test_wheel.c && ./a.out */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../firmware/target/hosted/shanling/wheel-q2.h"

#define F WHEEL_FINE

static int step(const char *config)
{
    FILE *f = fmemopen((void *)config, strlen(config), "r");
    assert(f);
    int s = wheel_read_step(f);
    fclose(f);
    return s;
}

/* Ticks in three turns, a unit per sample, over the 200 <-> 1 seam */
static int spin(int s, int dir)
{
    int a = 10 * F, ticks = 0;
    for (int k = 1; k <= 600; k++)
        ticks += wheel_ticks(&a, ((9 + dir * k) % 200 + 200) % 200 + 1, s);
    return ticks;
}

int main(void)
{
    const int s = 20 * F; /* Wheel Sensitivity 70% */
    int a = 50 * F;
    assert(wheel_ticks(&a, 61, s) == 0 && a == 50 * F);  /* under a step: nothing */
    assert(wheel_ticks(&a, 70, s) == 1 && a == 70 * F);
    assert(wheel_ticks(&a, 69, s) == 0 && a == 70 * F);  /* jitter back: no tick */
    assert(wheel_ticks(&a, 111, s) == 2 && a == 110 * F); /* remainder carries */
    assert(wheel_ticks(&a, 70, s) == -2 && a == 70 * F);
    a = 195 * F;                                         /* across 200 -> 1 */
    assert(wheel_ticks(&a, 15, s) == 1 && a == 15 * F);
    assert(wheel_ticks(&a, 199, s) == 0 && a == 15 * F);  /* 16 back: nothing */
    assert(wheel_ticks(&a, 195, s) == -1 && a == 195 * F);
    a = 10 * F;
    assert(wheel_ticks(&a, 110, s) == 5 && a == 110 * F); /* half a turn: forwards */

    /* Q2 Pod's config.ini, its travel per tick (test/playback.py's SIXTEENTHS) */
    assert(wheel_read_step(NULL) == 448);
    assert(step("") == 448);
    assert(step("[Q2POD]\nWHEELSENSITIVITY=50\n") == 448);
    assert(step("[IPOD]\nLIGHT=1\n[Q2POD]\nWHEELSENSITIVITY=70\nX=1\n") == 320);
    assert(step("WHEELSENSITIVITY=100\r\n") == 224);
    assert(step("WHEELSENSITIVITY=130\n") == 172);
    assert(step("WHEELSENSITIVITY=200") == 112);
    static const char *const bad[] = { "0", "40", "210", "135", "-50", "050", " 130", "130x", "abc", "" };
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
        char config[64];
        snprintf(config, sizeof config, "WHEELSENSITIVITY=%s\n", bad[i]);
        assert(step(config) == 448);
    }

    /* The same ticks a turn as Q2 Pod's wheel at each sensitivity */
    assert(spin(448, 1) == 21 && spin(448, -1) == -21);
    assert(spin(172, 1) == 55 && spin(172, -1) == -55);
    assert(spin(step("WHEELSENSITIVITY=150\n"), 1) == 64);
    assert(spin(step("WHEELSENSITIVITY=160\n"), 1) == 68);
    assert(spin(112, 1) == 85 && spin(112, -1) == -85);
    puts("wheel ok");
    return 0;
}
