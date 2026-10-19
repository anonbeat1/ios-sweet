#!/usr/bin/env python3
"""Minimal boot.img header v3 (Spacewar: dtb + base come from vendor_boot)."""
import struct
import sys

PAGE = 4096


def pad(b):
    return b + b'\0' * (-len(b) % PAGE)


kernel, ramdisk, out = (open(p, 'rb').read() if i < 2 else p for i, p in enumerate(sys.argv[1:4]))
os_version = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x1600016b  # stock 2501081938 value
hdr = struct.pack('<8s9I', b'ANDROID!', len(kernel), len(ramdisk), os_version, 1580, 0, 0, 0, 0, 3)
hdr += b'\0' * 1536  # cmdline (vendor_boot supplies the real one)
open(out, 'wb').write(pad(hdr) + pad(kernel) + pad(ramdisk))
