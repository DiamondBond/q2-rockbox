/* Host check of the Q2 wheel's tick logic: cc test_wheel.c && ./a.out */
#include <assert.h>
#include <stdio.h>
#include "../../firmware/target/hosted/shanling/wheel-q2.h"

int main(void)
{
    int a = 50;
    assert(wheel_ticks(&a, 61) == 0 && a == 50);  /* under a step: nothing */
    assert(wheel_ticks(&a, 68) == 1 && a == 68);
    assert(wheel_ticks(&a, 67) == 0 && a == 68);  /* jitter back: no tick */
    assert(wheel_ticks(&a, 105) == 2 && a == 104); /* remainder carries */
    assert(wheel_ticks(&a, 68) == -2 && a == 68);
    a = 195;                                      /* across 200 -> 1 */
    assert(wheel_ticks(&a, 13) == 1 && a == 13);
    assert(wheel_ticks(&a, 199) == 0 && a == 13);  /* 14 back: nothing */
    assert(wheel_ticks(&a, 195) == -1 && a == 195);
    a = 10;
    assert(wheel_ticks(&a, 110) == 5 && a == 100); /* half a turn: forwards */
    puts("wheel ok");
    return 0;
}
