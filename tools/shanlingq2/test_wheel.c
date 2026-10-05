/* Host check of the Q2 wheel's tick logic: cc test_wheel.c && ./a.out */
#include <assert.h>
#include <stdio.h>
#include "../../firmware/target/hosted/shanling/wheel-q2.h"

int main(void)
{
    int a = 50;
    assert(wheel_ticks(&a, 59) == 0 && a == 50);  /* under a step: nothing */
    assert(wheel_ticks(&a, 60) == 1 && a == 60);
    assert(wheel_ticks(&a, 59) == 0 && a == 60);  /* jitter back: no tick */
    assert(wheel_ticks(&a, 85) == 2 && a == 80);  /* remainder carries */
    assert(wheel_ticks(&a, 60) == -2 && a == 60);
    a = 195;                                      /* across 200 -> 1 */
    assert(wheel_ticks(&a, 6) == 1 && a == 5);
    assert(wheel_ticks(&a, 196) == 0 && a == 5);    /* 9 back: nothing */
    assert(wheel_ticks(&a, 195) == -1 && a == 195);
    a = 10;
    assert(wheel_ticks(&a, 110) == 10 && a == 110); /* half a turn: forwards */
    puts("wheel ok");
    return 0;
}
