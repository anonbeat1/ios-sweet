#!/usr/bin/env python3
"""Raw image -> Android sparse image (fastboot flash), like AOSP img2simg.

  img2simg.py RAW OUT.simg        (self-check: img2simg.py --test)

Zero blocks become FILL 0 chunks, never DONT_CARE: the target partition holds
old data (Spacewar userdata was f2fs) and HFS+ expects zeros where its image
has them (journal, empty B-tree nodes).
"""
import struct, sys

BLK = 4096
RAW, FILL, DONT_CARE = 0xCAC1, 0xCAC2, 0xCAC3


def chunks(f, nblocks):
    """Yield (type, nblocks, data) runs of equal kind (data for RAW only)."""
    kind, run, data = None, 0, []
    for _ in range(nblocks):
        b = f.read(BLK).ljust(BLK, b'\0')
        k = FILL if b.count(0) == BLK else RAW
        if k != kind and run:
            yield kind, run, b''.join(data)
            run, data = 0, []
        kind = k
        run += 1
        if k == RAW:
            data.append(b)
            if run == 4096:		# 16 MiB RAW chunks keep memory bounded
                yield kind, run, b''.join(data)
                kind, run, data = None, 0, []
    if run:
        yield kind, run, b''.join(data)


def convert(src, dst):
    with open(src, 'rb') as f:
        f.seek(0, 2)
        size = f.tell()
        f.seek(0)
        assert size % BLK == 0, 'image size must be a multiple of 4096'
        nblocks = size // BLK
        # Two passes: the header needs the chunk count, and data never sits in RAM.
        nchunks = sum(1 for _ in chunks(f, nblocks))
    with open(src, 'rb') as f, open(dst, 'wb') as o:
        o.write(struct.pack('<IHHHHIIII', 0xED26FF3A, 1, 0, 28, 12, BLK, nblocks, nchunks, 0))
        for kind, run, data in chunks(f, nblocks):
            if kind == RAW:
                o.write(struct.pack('<HHII', RAW, 0, run, 12 + len(data)) + data)
            else:
                o.write(struct.pack('<HHIII', FILL, 0, run, 16, 0))
    return nblocks, nchunks


def expand(simg):
    """Inverse, for the self-check."""
    hdr = struct.unpack_from('<IHHHHIIII', simg, 0)
    assert hdr[0] == 0xED26FF3A
    off, out = 28, b''
    for _ in range(hdr[7]):
        kind, _, run, total = struct.unpack_from('<HHII', simg, off)
        body = simg[off + 12:off + total]
        out += body if kind == RAW else body[:4] * (run * BLK // 4)
        off += total
    assert len(out) == hdr[6] * BLK
    return out


if __name__ == '__main__':
    if sys.argv[1:] == ['--test']:
        import os, tempfile
        raw = b'\0' * BLK * 3 + os.urandom(BLK * 5000) + b'\0' * BLK + b'x' * BLK
        with tempfile.TemporaryDirectory() as d:
            open(f'{d}/r', 'wb').write(raw)
            print(convert(f'{d}/r', f'{d}/s'))
            assert expand(open(f'{d}/s', 'rb').read()) == raw
        print('img2simg ok')
    else:
        print('blocks %d, chunks %d' % convert(sys.argv[1], sys.argv[2]))
