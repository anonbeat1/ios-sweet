#!/usr/bin/env python3
"""Read files from the phone over the USB link (kernel file server, xnu 0100).

  sweet-pull.py ls DIR                 list a directory
  sweet-pull.py get FILE [OUT]         copy one file (default: ./basename)
  sweet-pull.py mirror DIR OUTDIR      copy a directory tree
  sweet-pull.py logs OUT.logarchive    logd's store + uuidtext as a log archive,
                                       then: log show --archive OUT.logarchive
  sweet-pull.py put LOCAL PATH [MODE]  install one file (mode octal, default 644)
  sweet-pull.py untar TAR[.zst]        install a tar's dirs/files/symlinks under /

Protocol: TCP 192.168.7.2:5141, one request per connection ("LS /dir",
"GET /file", "PUT mode size /file" + bytes, "MKDIR mode /dir",
"SYMLINK /link target"); reply "OK" / "OK <size>" or "ERR <errno>".
"""
import os
import plistlib
import socket
import subprocess
import sys
import tarfile

HOST, PORT = '192.168.7.2', 5141


def request(line):
    s = socket.create_connection((HOST, PORT), timeout=30)
    s.sendall(line.encode() + b'\n')
    f = s.makefile('rb')
    head = f.readline().decode().strip()
    if not head.startswith('OK'):
        s.close()
        raise OSError(f'{line}: {head or "no reply"}')
    return s, f, head


def ls(path):
    s, f, _ = request(f'LS {path}')
    out = []
    for raw in f:
        t, size, name = raw.decode(errors='surrogateescape').rstrip('\n').split(' ', 2)
        out.append((t, int(size), name))
    s.close()
    return out


def get(path, dest):
    s, f, head = request(f'GET {path}')
    size = int(head.split()[1])
    os.makedirs(os.path.dirname(dest) or '.', exist_ok=True)
    left = size
    with open(dest, 'wb') as o:
        while left > 0:
            b = f.read(min(left, 1 << 20))
            if not b:
                raise OSError(f'{path}: short read, {left} of {size} bytes missing')
            o.write(b)
            left -= len(b)
    s.close()
    return size


def command(line, body=b''):
    s = socket.create_connection((HOST, PORT), timeout=60)
    s.sendall(line.encode() + b'\n' + body)
    reply = s.makefile('rb').readline().decode().strip()
    s.close()
    if reply != 'OK':
        raise OSError(f'{line}: {reply or "no reply"}')


def put(data, path, mode=0o644):
    command(f'PUT {mode:o} {len(data)} {path}', data)


def untar(tarpath):
    if tarpath.endswith('.zst'):
        src = subprocess.Popen(['zstd', '-dc', tarpath], stdout=subprocess.PIPE).stdout
    else:
        src = open(tarpath, 'rb')
    n = 0
    tf = tarfile.open(fileobj=src, mode='r|')
    for m in tf:
        path = '/' + (m.name[2:] if m.name.startswith('./') else m.name.lstrip('/'))
        if path == '/':
            continue
        if m.isdir():
            command(f'MKDIR {m.mode & 0o7777:o} {path}')
        elif m.issym():
            command(f'SYMLINK {path} {m.linkname}')
        elif m.isfile():
            put(tf.extractfile(m).read(), path, m.mode & 0o7777)
        else:
            print(f'skip {path} (type {m.type})', file=sys.stderr)
            continue
        n += 1
        if n % 500 == 0:
            print(n, path, flush=True)
    print(n, 'entries installed')


def mirror(path, outdir):
    total = 0
    for t, size, name in ls(path):
        src, dst = f'{path.rstrip("/")}/{name}', os.path.join(outdir, name)
        if t == 'd':
            total += mirror(src, dst)
        elif t == 'f':
            try:
                total += get(src, dst)
            except OSError as e:
                print(f'skip {e}', file=sys.stderr)
    return total


def logs(out):
    n = mirror('/private/var/db/diagnostics', out)
    n += mirror('/private/var/db/uuidtext', out)
    info = os.path.join(out, 'Info.plist')
    if not os.path.exists(info):
        with open(info, 'wb') as o:
            plistlib.dump({'OSArchiveVersion': 4}, o)
    print(f'{n} bytes -> {out}; now: log show --archive {out}')


def main(a):
    if len(a) >= 2 and a[0] == 'ls':
        for t, size, name in ls(a[1]):
            print(t, size, name)
    elif len(a) >= 2 and a[0] == 'get':
        print(get(a[1], a[2] if len(a) > 2 else os.path.basename(a[1])), 'bytes')
    elif len(a) == 3 and a[0] == 'mirror':
        print(mirror(a[1], a[2]), 'bytes')
    elif len(a) == 2 and a[0] == 'logs':
        logs(a[1])
    elif len(a) >= 3 and a[0] == 'put':
        put(open(a[1], 'rb').read(), a[2], int(a[3], 8) if len(a) > 3 else 0o644)
    elif len(a) == 2 and a[0] == 'untar':
        untar(a[1])
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
