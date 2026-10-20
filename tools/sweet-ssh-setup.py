#!/usr/bin/env python3
"""Install the Procursus rootless bootstrap (/var/jb) and an sshd launchd job
on the phone over the kernel file server (xnu 0101), then:
    ssh -i research/ssh/id_ed25519 root@192.168.7.2      (or password alpine)

  sweet-ssh-setup.py BOOTSTRAP.tar.zst PUBKEY

Changes outside /var/jb: /bin/sh -> /var/jb/bin/sh (root's login shell, absent
on iOS), /var/root/.ssh/authorized_keys, and an sshd job (port 22) added to
/System/Library/xpc/launchd.plist (read unsigned with launchd_unsecure_cache=1).
The phone's previous launchd.plist is kept as launchd.plist.pre-ssh, on the
phone and in research/.
"""
import importlib.util
import os
import plistlib
import sys

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('pull', os.path.join(here, 'sweet-pull.py'))
pull = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pull)

LAUNCHD = '/System/Library/xpc/launchd.plist'
# launchd ignored the job under /var/jb/Library/LaunchDaemons (no log line at all)
JOB_KEY = '/System/Library/LaunchDaemons/com.openssh.sshd.plist'
JOB = {
    'Label': 'com.openssh.sshd',
    'Program': '/var/jb/bin/sh',
    'ProgramArguments': ['/var/jb/bin/sh', '/var/jb/usr/libexec/sshd-keygen-wrapper', '-i'],
    'Sockets': {'SSHListener': {'SockServiceName': '22'}},
    'inetdCompatibility': {'Wait': False},
    'SessionCreate': True,
    'POSIXSpawnType': 'Interactive',
    'ProcessType': 'Interactive',
    'StandardErrorPath': '/dev/null',
}


def main(bootstrap, pubkey):
    pull.untar(bootstrap)
    pull.command('SYMLINK /bin/sh /var/jb/bin/sh')
    pull.command('MKDIR 700 /var/root/.ssh')
    pull.put(open(pubkey, 'rb').read(), '/var/root/.ssh/authorized_keys', 0o600)

    research = os.path.join(here, '..', 'research')
    backup = os.path.join(research, 'launchd.plist.pre-ssh')
    if not os.path.exists(backup):
        pull.get(LAUNCHD, backup)
        pull.put(open(backup, 'rb').read(), LAUNCHD + '.pre-ssh')
    cur = os.path.join(research, 'launchd.plist.phone')
    pull.get(LAUNCHD, cur)
    with open(cur, 'rb') as f:
        plist = plistlib.load(f)
    plist['LaunchDaemons'][JOB_KEY] = JOB
    data = plistlib.dumps(plist, fmt=plistlib.FMT_BINARY)
    pull.put(data, LAUNCHD)
    print(f'sshd job added to {LAUNCHD} ({len(data)} bytes); reboot the phone, then ssh')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
