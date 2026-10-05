/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 Diamond Bond
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

/* Startup code for Shanling Q2 binaries. The Q2's glibc 2.28 is nan2008 and
 * Debian's crt1.o and libc_nonshared.a are not, so this replaces both: glibc's
 * MIPS __start, and the libc_nonshared wrappers a program may use. */
#include <stdlib.h>
#include <sys/stat.h>

/* glibc's MIPS __start: main, argc, argv, no init or fini, the loader's
 * rtld_fini, stack_end.
 * ponytail: no init, so .init_array (C++ or constructor attributes) does not
 * run; pass a function walking __init_array_start..end if one is ever needed. */
__asm__(".set noreorder\n.globl __start\n__start:\n"
        "lui $28, %hi(_gp)\naddiu $28, $28, %lo(_gp)\n"
        "move $31, $0\nlw $5, 0($29)\naddiu $6, $29, 4\n"
        "li $8, -8\nand $29, $29, $8\naddiu $29, $29, -32\n"
        "lui $4, %hi(main)\naddiu $4, $4, %lo(main)\nmove $7, $0\n"
        "sw $0, 16($29)\nsw $2, 20($29)\nsw $29, 24($29)\n"
        "lw $25, %call16(__libc_start_main)($28)\njalr $25\nnop\n"
        "1: b 1b\nnop\n.set reorder\n");

/* crt1.o's marker: without it glibc takes the program for a libc5-era one and
 * gives it old-layout stdio FILEs, which crash exit() */
const int _IO_stdin_used = 0x20001;

void *__dso_handle = 0;

int __cxa_atexit(void (*)(void *), void *, void *);
int atexit(void (*f)(void))
{
    return __cxa_atexit((void (*)(void *))f, 0, __dso_handle);
}

int __xstat(int, const char *, struct stat *);
int __fxstat(int, int, struct stat *);
int __lxstat(int, const char *, struct stat *);
int __fxstatat(int, int, const char *, struct stat *, int);
int __xmknod(int, const char *, mode_t, dev_t *);

int stat(const char *p, struct stat *s) { return __xstat(_STAT_VER, p, s); }
int fstat(int fd, struct stat *s) { return __fxstat(_STAT_VER, fd, s); }
int lstat(const char *p, struct stat *s) { return __lxstat(_STAT_VER, p, s); }
int fstatat(int d, const char *p, struct stat *s, int f)
{
    return __fxstatat(_STAT_VER, d, p, s, f);
}
int mknod(const char *p, mode_t m, dev_t d) { return __xmknod(_MKNOD_VER, p, m, &d); }
