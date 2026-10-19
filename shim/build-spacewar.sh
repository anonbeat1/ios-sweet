#!/bin/bash
# Spacewar probe shim (macOS: Apple clang + Homebrew lld) -> out/spacewar-boot.img (boot.img v3).
set -euo pipefail
cd "$(dirname "$0")"
PATH=/opt/homebrew/opt/lld/bin:$PATH
CC="clang --target=aarch64-none-elf"
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
mkdir -p out/sw
for f in start.S spacewar.c console.c; do
	$CC $CFLAGS -c $f -o out/sw/${f%.*}.o
done
ld.lld -T shim.ld -N --build-id=none out/sw/start.o out/sw/spacewar.o out/sw/console.o -o out/sw/shim.elf
ld.lld -T shim.ld -N --build-id=none --oformat binary out/sw/start.o out/sw/spacewar.o out/sw/console.o -o out/sw/Image
# Same rule as build.sh (no relocation pass): linked at another base, only image_size may differ.
sed 's/^\t\. = 0;/\t. = 0x200000;/' shim.ld > out/sw/moved.ld
ld.lld -T out/sw/moved.ld -N --build-id=none --oformat binary out/sw/start.o out/sw/spacewar.o out/sw/console.o -o out/sw/Image.moved
cmp -s <(tail -c +25 out/sw/Image) <(tail -c +25 out/sw/Image.moved) || { echo "error: image is not position independent" >&2; exit 1; }
# ABL copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
size=$(python3 -c "import struct,sys; print(struct.unpack_from('<Q', open(sys.argv[1],'rb').read(), 16)[0])" out/sw/Image)
truncate -s "$size" out/sw/Image
(cd out/sw && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
python3 ../tools/mkbootimg_v3.py out/sw/Image out/sw/ramdisk.bin out/spacewar-boot.img
ls -l out/sw/Image out/spacewar-boot.img
