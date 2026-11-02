#!/usr/bin/env python3
"""Host regression check: python3 tools/shanlingq2/test_tick.py (requires cc)."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]

source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define TIMER_FREQ 1000000
#define panicf(...) abort()
static void call_tick_tasks(void);
#include "firmware/target/hosted/kernel-unix.c"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pid_t tids[1024];
static int ticks, distinct, running;
static bool overlapped, wrong_mask;

static void call_tick_tasks(void) {
    sigset_t mask, all;
    pid_t tid = gettid();
    int d = 0, n;
    if (__atomic_fetch_add(&running, 1, __ATOMIC_SEQ_CST))
        overlapped = true;
    pthread_sigmask(SIG_BLOCK, NULL, &mask);
    sigfillset(&all);
    pthread_mutex_lock(&lock);
    for (int sig = 1; sig < NSIG; sig++) {
        /* faults must reach the panic screen, all else stays blocked; the
         * kernel never lets SIGKILL and SIGSTOP be blocked */
        bool fault = sig == SIGSEGV || sig == SIGBUS || sig == SIGILL ||
                     sig == SIGFPE;
        if (sig != SIGKILL && sig != SIGSTOP && sigismember(&all, sig) == 1 &&
            sigismember(&mask, sig) != !fault)
            wrong_mask = true;
    }
    while (d < distinct && tids[d] != tid)
        d++;
    if (d == distinct)
        tids[distinct++] = tid;
    n = ++ticks;
    pthread_mutex_unlock(&lock);
    /* a wakeup 20 ticks late: they must still run, one after the other */
    if (n == 10)
        usleep(200000);
    __atomic_fetch_sub(&running, 1, __ATOMIC_SEQ_CST);
}

static long long elapsed_ms(const struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000LL +
           (now.tv_nsec - start->tv_nsec) / 1000000;
}

int main(void) {
    struct timespec start;
    sigset_t mask;
    clock_gettime(CLOCK_MONOTONIC, &start);
    tick_start(10);
    /* the caller's mask is back, and it holds what wait_for_interrupt needs */
    pthread_sigmask(SIG_BLOCK, NULL, &mask);
    assert(!sigismember(&mask, SIGIO));
    assert(pthread_mutex_trylock(&wfi_mtx) == EBUSY);
    while (elapsed_ms(&start) < 1000)
        wait_for_interrupt();
    pthread_mutex_lock(&lock);
    long long ms = elapsed_ms(&start);
    fprintf(stderr, "%d ticks in %lld ms, %d distinct tids, wrong mask: %d, "
            "overlapped: %d\n", ticks, ms, distinct, wrong_mask, overlapped);
    assert(distinct == 1);
    assert(!wrong_mask);
    assert(!overlapped);
    assert(llabs(ticks - ms / 10) <= 10);
    return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    src, exe = tmp / "tick.c", tmp / "tick"
    for name in ("config.h", "system.h", "kernel.h", "button.h", "audio.h",
                 "panic.h", "timer.h"):
        (tmp / name).write_text("")
    src.write_text(source)
    subprocess.run(["cc", "-D_GNU_SOURCE", "-DSHANLING_Q2", "-Wall", "-Wextra",
                    "-Werror", "-fsanitize=undefined", "-I", str(tmp), "-I",
                    str(root), str(src), "-pthread", "-lrt", "-o", str(exe)],
                   check=True)
    subprocess.run([str(exe)], check=True, timeout=10)
print("tick thread: OK")
