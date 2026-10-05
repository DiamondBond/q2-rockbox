# Rockbox on the Shanling Q2: dual boot plan

## Context

The goal is Rockbox on the Q2 (Ingenic X2000, MIPS32r2 nan2008/fp64, Linux 5.15, glibc 2.28) next to Q2 Pod. It uses the iPod/H2-style dual boot: hold a key at power-on to switch, and the device boots whichever system was used last.

A native port is out. The X2000 has no Rockbox drivers, the DAC and MCU are proprietary, and the bootloader is never flashed. Rockbox runs **hosted** on the stock kernel instead, the same way as the xDuoo X3ii/X20, Eros Q (hosted), AGPTek Rocker and Sony NWZ ports.

Decisions made:
- **Separate repo** for the GPL port (a fork of rockbox, see Naming). It ships `rockbox.zip` to unzip onto the microSD card (`/mnt/mmc/.rockbox/`). The rootfs has only ~272 KB of slack (iPod) or ~12 KB (Stock), so Rockbox cannot live in it.
- **q2-pod** gets only a tiny MIT boot hook. It does nothing unless `/.rockbox/rockbox` is on the card.
- Entry: **hold Play/Pause at power-on** to switch systems, and the **last choice is remembered**. There is no menu row.

The boot flow today (`docs/boot.md`): `S10mdev` mounts the card at `/mnt/mmc` through `/etc/mdev/add_removesd.sh` (which touches `/tmp/mmc_add`). `S10mount_ubifs` mounts `/mnt/data`. `S11` draws the splash and loads the drivers. `S90play` runs `/release/bin/demo &`. demo then starts the `checkappprocess.sh` watchdog, which pgreps `/release/bin/demo`.

## Update (2026-10-05): kept separate from q2-pod for now

Part A is **deferred**. Q2 Pod and Rockbox on one device will be worked out later. Until then the Rockbox fork is standalone: M0 adds a dev loader in the fork (`tools/shanlingq2/mkupdate.py` or similar). It takes the stock `Q2 Firmware V1.32.zip`, makes a sha-pinned edit to `etc/init.d/S90play` so it runs a launcher, and repacks `update.tar` the way q2-pod's `tools/build.py` does (unsquashfs pseudo file, then mksquashfs lzo within the stock size). The launcher waits for `/tmp/mmc_add`, runs `/mnt/mmc/.rockbox/rockbox` with output logged to `rockbox.log` on the card, and falls back to `/release/bin/demo` if the binary is missing or exits. The stock rootfs has no adb, telnet or ssh, only a UART getty on `ttyS3`, so this loader is the only way to run code on the device. Keep the launcher contract (path, log, exit `0x51` = stock UI) so the q2-pod hook can adopt it later.

## Part A (deferred): q2-pod boot hook (this repo, first, because it is also the dev loader)

There is no shell on the device, so the hook doubles as the way to run any test binary from the card.

1. **`patch/boot.c` → `/usr/bin/q2boot`**, a few KB. Built like `q2video`: generalise `compile_helper` in `tools/build.py:399` to take a source and an output name, and add it to the `HELPER` inode loop (`tools/build.py:~603`) with `display_logo`'s metadata. Its logic:
   - Read the target from `/mnt/data/boot-target` (`rockbox`, or absent for Q2 Pod).
   - Check Play/Pause with `EVIOCGKEY` on `/dev/input/event1..3` (find the keycode in M0). If it is held, flip the target and save it.
   - Q2 Pod → `execl("/release/bin/demo")`. The watchdog still finds `/release/bin/demo`, since exec keeps the name.
   - Rockbox → wait up to 3 s for `/tmp/mmc_add`. If `/mnt/mmc/.rockbox/rockbox` is missing, fall back to demo. Otherwise do the platform setup that demo would have done (see M0), fork, exec it with stdout/stderr going to `/mnt/mmc/.rockbox/rockbox.log`, and wait.
   - When Rockbox exits, start demo. Exit code `0x51` ("boot Q2 Pod") also clears the target. A crash keeps the target but still drops to Q2 Pod, so the device is never stuck.
2. **`S90play`**: a sha-pinned edit in the style of `patch_watchdog` (`tools/build.py:428`) replaces `/release/bin/demo &` with `/usr/bin/q2boot &`.
3. **Size:** check that the Stock build still fits (about 12 KB of slack). Use no libc beyond what `video.c` declares.
4. **Tests:** a host build of `boot.c`'s decision function, in the same style as `video.c` in `test/peq.py`, covering the target × key × card present/missing matrix. `test/build.py` asserts that `S90play` is patched and `q2boot` is in the image.
5. **Docs:** a "Rockbox" section in `docs/boot.md` (flow, file, exit codes) and a short README entry with no emoji. Run `tools/format.sh`, then a /ponytail-review sub-agent before the build (per memory).

## Naming and repo setup (for official recognition)

The repo name does not matter to Rockbox. What gets a port recognised is landing it upstream: patches go through Gerrit (gerrit.rockbox.org) on top of master, with GPLv2 headers and Rockbox's coding style. A hosted port that works is then listed as an "unstable" target. To make that path as short as possible:
- **Fork `github.com/Rockbox/rockbox` as `DiamondBond/rockbox`** (keep the name), with work on branch **`shanlingq2`**. Clone it to `~/src/rockbox`. The history stays upstream's, so commits rebase and push straight to Gerrit.
- Target id **`shanlingq2`** and model name "Shanling Q2", matching the existing native `shanlingq1`. The target dirs are `firmware/target/hosted/shanling/`, plus `firmware/export/config/shanlingq2.h`.
- The Q2 Pod hook stays in q2-pod. Rockbox only needs a "Boot Q2 Pod" exit code, which is target-local and harmless upstream.
- Announce the port in the Rockbox forums' New Ports board once M2 plays audio.

## Handoff to the new instance (`~/src/rockbox`)

It has no context from this repo, so start it with: "Read `~/git/q2-ringnav/docs/boot.md`, `docs/internals.md` (Videos, DAC, battery, encoder sections) and `patch/video.c`, and the unpacked stock rootfs via `unsquashfs` on `~/git/q2-ringnav/build/stock.squashfs`. Then do M0." Part A is built here in q2-ringnav. The Rockbox instance only relies on its contract: binary `/mnt/mmc/.rockbox/rockbox`, log to `rockbox.log`, exit `0x51` boots Q2 Pod.

## Part B: Rockbox port (fork `DiamondBond/rockbox`, branch `shanlingq2`, hosted)

Modelled on the closest hosted Ingenic Linux targets (`firmware/target/hosted/xduoo`, `aigo`, and the shared `pcm-alsa.c`, `alsa-controls.c`, `lcd-linuxfb`, `power-linux`/`sysfs.c`).

- **M0: discovery and toolchain** (read-only reverse engineering of stock `demo`, same methods as this repo):
  - demo's `platform_init`: `cmd_mcu write_firmware /usr/data/libmcu-bare.bin`, `cmd_mcu bootup`, DAC power (`mclSetDacPwr`), amp GPIOs, backlight. Rockbox mode skips demo, so this has to be replicated. **This is the biggest risk.**
  - The power-off sequence, the backlight sysfs path, the Play keycode and the evdev node of each key, and the encoder knob's sysfs notifier (`encoderknob_thread_run` `0x6256a0`).
  - Toolchain: clang + ld.lld against the rootfs's own libs, as `compile_helper` already proves, plus glibc 2.28 and alsa-lib headers. Done when a hello binary draws to fb0 through the Part A hook.
- **M1: display and input.** A 375×320 landscape LCD rotated clockwise onto the 320×375 BGRA two-page fb0 with pan (reuse the logic in `patch/video.c`; it is our own code and relicensable). Keys from evdev, and the wheel as `BUTTON_SCROLL_FWD/BACK` with an iPod-style keymap (Return = Menu, Centre = Select, Play, side keys 222/223 = prev/next). The touchscreen (`event2`) stays unused. Done 2026-10-05: wheel 20 ticks a turn, no acceleration (Rockbox doesn't need it); lists inset from the glass corners (`DEFAULT_UI_VIEWPORT`).
- **M2: audio.** ALSA `hw:1,0`. DAC ioctls on `/dev/shanling_dac`: PCM mode `0xc0044d1b`, unmute `0xc0044d1f`, volume `0xc0044d00` (from `docs/internals.md`). Mute at stop. Done 2026-10-05 (the DAC is ALSA card 0, `plughw:0,0`): DAC held at 0 dB, volume in software; Gain (Low/High) as the DAC power mode setting.
- **M3: power.** Battery from `cw2015` sysfs (several board-rev paths, as stock probes) and charging from `/sys/class/power_supply/battery/status`. Backlight, sleep, power off. Plugging in USB exits with `0x51` so stock demo handles mass storage. A "Boot Q2 Pod" menu item exits `0x51` too.
- **M4: release.** Default theme and config sized for 375×320, a `rockbox.zip` release with install notes. Double-buffer fb0 (draw the hidden page, then pan, as stock demo does) to stop the tearing seen when scrolling. Upstreaming to rockbox.org can come later, if wanted.

Out of scope for v1: Bluetooth output (bluealsa), touch, USB mass storage inside Rockbox.

## Status (2026-10-05) and remaining work

Commits on `shanlingq2` (pushed): M0 `33168c4082`, M1 `9f9ef4b166`, M2 `3ee25b675b`. Platform notes from reverse engineering are in `tools/shanlingq2/README`; read it first. The stock rootfs is unpacked in `/tmp/q2root/squashfs-root` and the annotated demo disassembly in `/tmp/q2re/demo.s` (both regenerable: `unsquashfs` on `~/git/q2-ringnav/build/stock.squashfs`, `/tmp/q2re/ann.py`).

Build and run:
- Sysroot: `~/.cache/q2/sysroot` (`tools/shanlingq2/mksysroot.sh 'Q2 Firmware V1.32.zip' DIR`; the ZIP is `~/git/q2-ringnav/Q2 Firmware V1.32.zip`).
- `cd build-q2 && Q2_SYSROOT=$HOME/.cache/q2/sysroot ../tools/configure --target=shanlingq2 --type=n && make -j$(nproc) && make zip`. Clang 22 + ld.lld; the full tree, codecs and plugins build with no errors.
- Checks without the device: `cc tools/shanlingq2/test_wheel.c && ./a.out`; `~/.cache/q2/qemu-mipsel-static -L /tmp/q2root/squashfs-root build-q2/rockbox.elf` runs startup up to opening `/dev/fb0` (use `-strace`, kill it after; it waits forever in the panic screen).
- Install: the user plugs the Q2 in (from the stock OS) as `/dev/sdb`, label `Q2`; `udisksctl mount -b /dev/sdb`, `unzip -o build-q2/rockbox.zip -d /run/media/diamond/Q2`, `sync`, `udisksctl unmount`. Delete `.rockbox/config.cfg` only when a changed default must take effect. Only the user can test on the device.
- The dev loader (`update.tar`, V1.3R) is flashed. Escapes to the stock OS: hold Return at power-on; hold Play (shutdown exits `0x51`); or boot without the card. `.rockbox/rockbox.log` ends in `exit N`.

Verified on the device: display (rotation, corner inset), keys, wheel direction, playback incl. high sample rates, volume, the shutdown and boot escapes, brightness 1..20 and the screen off/on cycle (M3).

Done since (M3, M4):
- M3: backlight through `/dev/jz_pwm` (the ioctls libhardware2 makes; verified). Battery from `cw2015_capacity`. The key or wheel touch that wakes the screen only wakes it (`DEFAULT_BL_FILTER_FIRST_KEYPRESS`). USB storage: shut down to stock (`pc_link` stays 0 under Rockbox, so plugging in cannot be detected). Settings > System > "Boot stock OS". Shutting down leaves the screen on for stock. Headphone detection skipped (optional).
- M4: fb0 double-buffered in `lcd-q2.c` (draw the hidden page, pan, wait for vsync, copy to the other page). cabbiev2 for 375x320: a WPS, and an SBS made the Q2's default (`DEFAULT_SBSNAME`) with battery, clock and volume inside the glass and the lists' viewport; backdrops recomposed from the 400x240 ones. Plugins without a 375x320 layout stay left out. Install notes are in `tools/shanlingq2/README`.
- A simulator build works: `Q2_SYSROOT` isn't needed with `--type=s` (`build-q2sim`, `make install`, then `./rockboxui`). Screens can be captured headless with Xvfb and xdotool (keys need a 150 ms hold).

Final test passed on the device (2026-10-05): no tearing, theme, battery %, Boot stock OS. Committed and pushed (`b7a4e3a860`).

Next, in order:
1. Check on the device: the wake-up fix (the first key or wheel touch on a dark screen only lights it; installed, untested) and the Gain switch (M2, untested).
2. Part A (in q2-ringnav): the q2-pod boot hook is built and tested on the host, but not yet committed or released (2026-10-05). `patch/boot.c` → `/usr/bin/q2boot` checks Play/Pause, and the launcher in S90play (`BOOT_HOOK` in `tools/build.py`) keeps this fork's launcher contract. The choice lives in `/mnt/data/boot-target`. Exits, including 0x51, start demo but don't change the choice. Once released, the dev loader isn't needed and the README should point to Q2 Pod.
3. Optional: pause on headphone unplug (GPIO PA07/PA08); 375x320 layouts for the left-out plugins; USB storage from inside Rockbox.
4. Announce in the Rockbox forums' New Ports board; later, upstream through Gerrit (the simulator build helps review).

## Verification


- Part A: `test/patch.py`/`test/build.py`/the new host test pass; `tools/build.py` builds both variants within the size limit. Without `.rockbox` on the card, boot is unchanged. With a test binary there: holding Play boots it, its exit returns to Q2 Pod, and the next boot follows the remembered choice. The user tests on the device.
- Part B: each milestone is checked through `rockbox.log` on the card and on the device. Build with `tools/configure` for target `shanlingq2`.

## Order

Part A first, released as a normal Q2 Pod version when the user asks. Then M0 → M4 in `~/src/rockbox`. Part A can run in parallel with M0, because M0 needs the hook to run anything on the device.
