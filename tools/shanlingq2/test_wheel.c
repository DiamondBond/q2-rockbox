/* Host check of the Q2 wheel's tick logic: cc test_wheel.c && ./a.out */
#include <assert.h>
#include <stdio.h>
#include "../../firmware/target/hosted/shanling/wheel-q2.h"

int main(void)
{
    int a = 50;
    assert(wheel_ticks(&a, 61) == 0 && a == 50);  /* under a step: nothing */
    assert(wheel_ticks(&a, 62) == 1 && a == 62);
    assert(wheel_ticks(&a, 61) == 0 && a == 62);  /* jitter back: no tick */
    assert(wheel_ticks(&a, 87) == 2 && a == 86);  /* remainder carries */
    assert(wheel_ticks(&a, 62) == -2 && a == 62);
    a = 195;                                      /* across 200 -> 1 */
    assert(wheel_ticks(&a, 7) == 1 && a == 7);
    assert(wheel_ticks(&a, 196) == 0 && a == 7);    /* 11 back: nothing */
    assert(wheel_ticks(&a, 195) == -1 && a == 195);
    a = 10;
    assert(wheel_ticks(&a, 110) == 8 && a == 106); /* half a turn: forwards */
    puts("wheel ok");
    return 0;
}
