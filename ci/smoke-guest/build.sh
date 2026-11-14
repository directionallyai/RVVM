#!/bin/sh
# Builds the CI smoke guest for RVVM: OpenSBI (fw_jump.bin) and a tiny Linux Image with an embedded initramfs whose only
# program is init.c. Needs a Debian/Ubuntu host with gcc-riscv64-linux-gnu and libc6-dev-riscv64-cross.
# usage: build.sh OUTDIR   (writes OUTDIR/fw_jump.bin, OUTDIR/Image, OUTDIR/Image-tick, OUTDIR/expected.txt)
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$(mkdir -p "${1:?usage: build.sh OUTDIR}" && cd "$1" && pwd)"
WORK="${WORK:-$(mktemp -d)}"
KERNEL=linux-6.12.116
KERNEL_SHA256=cb3ed3dfdd47e6ade49dc933db334fd86e24cadee920da612c262b0e9b6a511b
OPENSBI_TAG=v1.7
CROSS=riscv64-linux-gnu-
cd "$WORK"
[ -f $KERNEL.tar.xz ] || curl -fsSL -o $KERNEL.tar.xz https://cdn.kernel.org/pub/linux/kernel/v6.x/$KERNEL.tar.xz
echo "$KERNEL_SHA256  $KERNEL.tar.xz" | sha256sum -c -
[ -d $KERNEL ] || tar xf $KERNEL.tar.xz
${CROSS}gcc -O2 -static -o init "$HERE/init.c"
printf 'dir /dev 0755 0 0\nnod /dev/console 0600 0 0 c 5 1\ndir /proc 0755 0 0\nfile /init %s/init 0755 0 0\n' "$WORK" > initramfs.list
cd $KERNEL
make -s ARCH=riscv CROSS_COMPILE=$CROSS tinyconfig
scripts/config --enable 64BIT --enable MMU --enable PRINTK --enable TTY --enable SERIAL_8250 --enable SERIAL_8250_CONSOLE \
  --enable SERIAL_OF_PLATFORM --enable BLK_DEV_INITRD --enable BINFMT_ELF --enable PROC_FS --enable RISCV_SBI --enable FPU \
  --enable SOC_VIRT --enable SIFIVE_PLIC --enable RISCV_TIMER --enable POWER_RESET --enable POWER_RESET_SYSCON \
  --enable POWER_RESET_SYSCON_POWEROFF --enable MFD_SYSCON --set-str INITRAMFS_SOURCE "$WORK/initramfs.list" \
  --set-str CMDLINE "console=ttyS0 earlycon=sbi" --enable CMDLINE_FORCE
make -s ARCH=riscv CROSS_COMPILE=$CROSS olddefconfig
make -s ARCH=riscv CROSS_COMPILE=$CROSS -j"$(nproc)" Image
cp arch/riscv/boot/Image "$OUT/Image"
# A second Image whose init is the ticker (ci/snapshot_test.py): only the embedded initramfs differs, so this relinks quickly
cd "$WORK"
${CROSS}gcc -O2 -static -o tick "$HERE/tick.c"
printf 'dir /dev 0755 0 0\nnod /dev/console 0600 0 0 c 5 1\ndir /proc 0755 0 0\nfile /init %s/tick 0755 0 0\n' "$WORK" > initramfs-tick.list
cd $KERNEL
scripts/config --set-str INITRAMFS_SOURCE "$WORK/initramfs-tick.list"
make -s ARCH=riscv CROSS_COMPILE=$CROSS -j"$(nproc)" Image
cp arch/riscv/boot/Image "$OUT/Image-tick"
cd "$WORK"
[ -d opensbi ] || git clone -q --depth 1 --branch $OPENSBI_TAG https://github.com/riscv-software-src/opensbi opensbi
make -s -C opensbi -j"$(nproc)" CROSS_COMPILE=$CROSS PLATFORM=generic
cp opensbi/build/platform/generic/firmware/fw_jump.bin "$OUT/fw_jump.bin"
python3 "$HERE/expected.py" > "$OUT/expected.txt"
ls -l "$OUT"
