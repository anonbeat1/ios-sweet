#!/bin/bash
# Spacewar probe shim (shim.c with -DSPACEWAR) (macOS: Apple clang + Homebrew lld) -> out/spacewar-boot.img (boot.img v3).
set -euo pipefail
cd "$(dirname "$0")"
PATH=/opt/homebrew/opt/lld/bin:$PATH
CC="clang --target=aarch64-none-elf"
CFLAGS="-DSPACEWAR -O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -fno-jump-tables -Wall -Wextra"
mkdir -p out/sw
SRCS="start.S shim.c console.c mmu.c fdt.c loader.c ufs.c touchprobe.c"
OBJS=$(for f in $SRCS; do echo out/sw/${f%.*}.o; done)
for f in $SRCS; do
	$CC $CFLAGS -c $f -o out/sw/${f%.*}.o
done
ld.lld -T shim.ld -N --build-id=none $OBJS -o out/sw/shim.elf
ld.lld -T shim.ld -N --build-id=none --oformat binary $OBJS -o out/sw/Image
# Same rule as build.sh (no relocation pass): linked at another base, only image_size may differ.
sed 's/^\t\. = 0;/\t. = 0x200000;/' shim.ld > out/sw/moved.ld
ld.lld -T out/sw/moved.ld -N --build-id=none --oformat binary $OBJS -o out/sw/Image.moved
cmp -s <(tail -c +25 out/sw/Image) <(tail -c +25 out/sw/Image.moved) || { echo "error: image is not position independent" >&2; exit 1; }
# ABL copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
size=$(python3 -c "import struct,sys; print(struct.unpack_from('<Q', open(sys.argv[1],'rb').read(), 16)[0])" out/sw/Image)
truncate -s "$size" out/sw/Image
# Ramdisk: an xnu payload (tools/mkpayload.py with an mkadt.py --spacewar ADT) if given
# as $1, else an empty newc cpio -> probe pages.
if [ -n "${1:-}" ]; then
	cp "$1" out/sw/ramdisk.bin
else
	(cd out/sw && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
fi
python3 ../tools/mkbootimg_v3.py out/sw/Image out/sw/ramdisk.bin out/spacewar-boot.img
ls -l out/sw/Image out/spacewar-boot.img
