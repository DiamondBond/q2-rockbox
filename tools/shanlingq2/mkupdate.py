#!/usr/bin/env python3
#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#
"""Shanling Q2 dev loader: the stock V1.32 update.tar with S90play running
/mnt/mmc/.rockbox/rockbox, output to rockbox.log beside it, instead of the stock
player, which starts when it exits or is missing. Flash the update.tar from the
card root: System settings > System Update > TF card update. Flashing stock
V1.32 puts the stock boot back. Requires squashfs-tools 4.6 or later.

Usage: mkupdate.py 'Q2 Firmware V1.32.zip' OUTDIR
"""
import hashlib, io, re, shlex, struct, subprocess, sys, tarfile, zipfile, pathlib

ZIP_SHA = '154c17822d09be001be35c03d2d3488424dee195221790bd70864480d55b0f00'
DEMO_SHA = '2c5f06142850b4fc168f82b44a81550cce0a5b4b9fe1c179dced4a08a3049138'
S90PLAY_SHA = 'a6a7ed7d9a10e38801f4a41ec6f3c0ce2bc07c00d213c9278785c5f8d4520e24'
# The updater refuses a firmware_v20.info version equal to demo's one version literal, so both
# carry this tag: it installs over stock, and stock V1.32 installs back over it.
TAG = b'V1.3R'
STOCK_START = b'    /release/bin/demo &\n'
# The card mounts after S90play (mdev runs as a daemon), so wait up to 3 s for it. demo keeps
# its argv[0], which checkappprocess.sh (started by demo) pgreps for; Rockbox runs without it.
# Exit 0x51 is "boot the stock player", which this loader does after any exit.
LAUNCHER = b'''    (
        for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
            [ -e /tmp/mmc_add ] && break
            usleep 200000
        done
        rb=/mnt/mmc/.rockbox
        if [ -f $rb/rockbox ]; then
            (cd $rb && exec ./rockbox) > $rb/rockbox.log 2>&1
            echo "exit $?" >> $rb/rockbox.log
        fi
        exec /release/bin/demo
    ) &
'''


def sha(b): return hashlib.sha256(b).hexdigest()


def check(ok, msg):
    if not ok: sys.exit('mkupdate: ' + msg)


def run(*args): return subprocess.check_output([str(a) for a in args])


def main(zip_path, out):
    out.mkdir(parents=True, exist_ok=True)
    check(not (out/'update.tar').exists(), 'output exists; use a fresh OUTDIR')
    raw = zip_path.read_bytes()
    check(sha(raw) == ZIP_SHA, 'not the stock V1.32 ZIP')
    with zipfile.ZipFile(io.BytesIO(raw)) as z:
        tar = z.read('Q2 Firmware V1.32/update.tar')
    with tarfile.open(fileobj=io.BytesIO(tar)) as t:
        meta = t.getmembers()
        blobs = {m.name: t.extractfile(m).read() for m in meta if m.isfile()}
    info = blobs['firmware_v20.info'].decode().splitlines()
    check(info[:2] == ['Shanling Q2', 'V1.32'], 'wrong model or version')
    for line in info[2:]:
        digest, name = line.split()
        check(hashlib.md5(blobs[name]).hexdigest() == digest, 'stock MD5 mismatch')

    sq = out/'stock.squashfs'
    sq.write_bytes(blobs['recovery-update/rootfs.squashfs'])
    def cat(path): return run('unsquashfs', '-cat', sq, path)
    s90 = cat('etc/init.d/S90play')
    check(sha(s90) == S90PLAY_SHA and s90.count(STOCK_START) == 1, 'unexpected S90play')
    (out/'S90play').write_bytes(s90.replace(STOCK_START, LAUNCHER))
    demo = cat('release/bin/demo')
    check(sha(demo) == DEMO_SHA and demo.count(b'V1.32\0') == 1, 'unexpected demo')
    (out/'demo').write_bytes(demo.replace(b'V1.32\0', TAG + b'\0'))

    # Pseudo-file round trip, as q2-pod's tools/build.py: every inode keeps its metadata.
    pseudo = out/'root.pseudo'
    run('unsquashfs', '-pf', pseudo, sq)
    p = pseudo.read_bytes()
    root = re.search(rb'^/ D (\d+) (\d+) (\d+) (\d+)$', p, re.M)
    check(root is not None, 'no root inode')
    t, mode, uid, gid = (x.decode() for x in root.groups())
    for path, src in ((b'etc/init.d/S90play', out/'S90play'), (b'release/bin/demo', out/'demo')):
        line = re.search(rb'^' + re.escape(path) + rb' R (\d+) (\d+) (\d+) (\d+) .+$', p, re.M)
        check(line is not None, f'no {path.decode()} inode')
        p = (p[:line.start()] + path + b' F ' + b' '.join(line.groups()) + b' cat ' +
             shlex.quote(str(src)).encode() + p[line.end():])
    pseudo.write_bytes(p)
    (out/'empty').mkdir(exist_ok=True)
    newsq = out/'rootfs.squashfs'
    epoch = struct.unpack_from('<I', sq.read_bytes(), 8)[0]
    run('mksquashfs', out/'empty', newsq, '-pf', pseudo, '-noappend', '-comp', 'lzo', '-b', '131072',
        '-Xcompression-level', '9', '-mkfs-time', epoch, '-root-time', t, '-root-mode', mode,
        '-root-uid', uid, '-root-gid', gid, '-processors', '1', '-no-progress', '-quiet')
    blobs['recovery-update/rootfs.squashfs'] = newsq.read_bytes()
    check(newsq.stat().st_size <= sq.stat().st_size, 'repacked rootfs exceeds stock size')

    blobs['firmware_v20.info'] = (f'Shanling Q2\n{TAG.decode()}\n' + ''.join(
        hashlib.md5(blobs[n]).hexdigest() + '  ' + n + '\n'
        for n in ['recovery-update/xImage', 'recovery-update/rootfs.squashfs'])).encode()
    with tarfile.open(out/'update.tar', 'w', format=tarfile.GNU_FORMAT) as t:
        for m in meta:
            data = blobs.get(m.name)
            if data is not None: m.size = len(data)
            t.addfile(m, io.BytesIO(data) if data is not None else None)
    print(f'{out/"update.tar"}: {sha((out/"update.tar").read_bytes())}, '
          f'{sq.stat().st_size - newsq.stat().st_size} bytes of rootfs slack')


if __name__ == '__main__':
    if len(sys.argv) != 3: sys.exit(__doc__)
    main(pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]).resolve())
