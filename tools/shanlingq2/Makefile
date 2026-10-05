#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#
# The M0 probe, linked like every Shanling Q2 binary: clang and ld.lld against
# the stock rootfs's libraries in SYSROOT (mksysroot.sh).
#   make SYSROOT=/path/to/sysroot
SYSROOT ?= sysroot
CFLAGS = --target=mipsel-linux-gnu --sysroot=$(SYSROOT) -march=mips32r2 -mabi=32 -mfp64 \
         -mnan=2008 -mabs=2008 -mabicalls -fno-pic -Os -Wall -Wextra -Werror
LDFLAGS = -m elf32ltsmip -e __start --dynamic-linker /lib/ld-linux-mipsn8.so.1 \
          --image-base=0x400000 -z noexecstack --hash-style=sysv --build-id=none -s

rockbox: crt.o hello.o
	ld.lld $(LDFLAGS) $^ $(SYSROOT)/lib/libc.so.6 $(SYSROOT)/lib/ld-linux-mipsn8.so.1 -o $@

%.o: %.c
	clang $(CFLAGS) -c $< -o $@

clean:
	rm -f rockbox *.o
.PHONY: clean
