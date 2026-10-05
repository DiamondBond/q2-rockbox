#!/bin/sh
#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#
# Builds the Shanling Q2 sysroot for clang: headers from Debian buster's mipsel
# packages (glibc 2.28, as on the Q2) and libraries from the stock rootfs, which
# binaries link against (the Q2's are nan2008, Debian's are not).
#
# Usage: mksysroot.sh 'Q2 Firmware V1.32.zip' SYSROOT
set -e
[ $# -eq 2 ] || { echo "usage: $0 'Q2 Firmware V1.32.zip' SYSROOT" >&2; exit 2; }
zip=$1
mkdir -p "$2/lib"
out=$(cd "$2" && pwd)
echo "154c17822d09be001be35c03d2d3488424dee195221790bd70864480d55b0f00  $zip" | sha256sum -c --quiet
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

while read -r sum deb; do
    curl -sSfo "$tmp/p.deb" "https://archive.debian.org/debian/pool/main/$deb"
    echo "$sum  $tmp/p.deb" | sha256sum -c --quiet
    (cd "$tmp" && ar x p.deb data.tar.xz && tar -xJf data.tar.xz -C "$out" ./usr/include)
done <<EOF
e28bac8637e98d78045f0fd05613ed2ab298fb1aa6387633b6d609119774d590 g/glibc/libc6-dev_2.28-10+deb10u1_mipsel.deb
5f6ca1c34bb326b38fa44838b1ed051c33a2591bf7bfa26276e54fc85c00427c l/linux/linux-libc-dev_4.19.249-2_mipsel.deb
729cf99def60974b1c893c21dbf242fded3b8c34547e547458c33fd48a41e3e7 a/alsa-lib/libasound2-dev_1.1.8-1_mipsel.deb
EOF
# -mnan=2008 picks this name; the stub list is the same as the legacy NaN one
gnu=$out/usr/include/mipsel-linux-gnu/gnu
cp "$gnu/stubs-o32_hard.h" "$gnu/stubs-o32_hard_2008.h"

unzip -p "$zip" 'Q2 Firmware V1.32/update.tar' | tar -xO recovery-update/rootfs.squashfs > "$tmp/rootfs"
unsquashfs -q -n -d "$tmp/root" "$tmp/rootfs" lib usr/lib > /dev/null
for l in lib/ld-linux-mipsn8.so.1 lib/libc.so.6 lib/libpthread.so.0 lib/libm.so.6 lib/libdl.so.2 lib/librt.so.1 \
         lib/libgcc_s.so.1 usr/lib/libasound.so.2 usr/lib/libts-0.0.so.0 usr/lib/libhardware2.so; do
    cp -L "$tmp/root/$l" "$out/lib/"
done
# the names -l looks for; -lgcc is libgcc_s, as the Q2 has no static libgcc
cd "$out/lib"
for l in c:libc.so.6 m:libm.so.6 pthread:libpthread.so.0 dl:libdl.so.2 rt:librt.so.1 \
         asound:libasound.so.2 gcc:libgcc_s.so.1; do
    ln -sf "${l#*:}" "lib${l%%:*}.so"
done
echo "sysroot: $out"
