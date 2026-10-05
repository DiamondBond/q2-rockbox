/* Host check of the Q2 wheel's tick logic: cc test_wheel.c && ./a.out */
#include <assert.h>
#include <stdio.h>
#include "../../firmware/target/hosted/shanling/wheel-q2.h"

int main(void)
{
    int a = 50;
    assert(wheel_ticks(&a, 61) == 0 && a == 50);  /* under a step: nothing */
    assert(wheel_ticks(&a, 70) == 1 && a == 70);
    assert(wheel_ticks(&a, 69) == 0 && a == 70);  /* jitter back: no tick */
    assert(wheel_ticks(&a, 111) == 2 && a == 110); /* remainder carries */
    assert(wheel_ticks(&a, 70) == -2 && a == 70);
    a = 195;                                      /* across 200 -> 1 */
    assert(wheel_ticks(&a, 15) == 1 && a == 15);
    assert(wheel_ticks(&a, 199) == 0 && a == 15);  /* 16 back: nothing */
    assert(wheel_ticks(&a, 195) == -1 && a == 195);
    a = 10;
    assert(wheel_ticks(&a, 110) == 5 && a == 110); /* half a turn: forwards */
    puts("wheel ok");
    return 0;
}
