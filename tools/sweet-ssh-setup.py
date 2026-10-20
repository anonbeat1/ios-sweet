#!/usr/bin/env python3
"""Install the Procursus rootless bootstrap (/var/jb) and an sshd launchd job
on the phone over the kernel file server (xnu 0101), then:
    ssh -i research/ssh/id_ed25519 root@192.168.7.2      (or password alpine)

  sweet-ssh-setup.py BOOTSTRAP.tar.zst PUBKEY

Inside /var/jb it also replaces usr/libexec/sshd-keygen-wrapper with
tools/sshd-keygen-wrapper.sh, which runs the Procursus postinst steps sshd
needs on the first connection (pwd_mkdb -p: libiosexec's getpwnam reads
/var/jb/etc/pwd.db, without it sshd says "Invalid user root"; etc/shells; root's
authorized_keys in /var/jb/var/root/.ssh), and gives root the iOS default
password (alpine, DES hash as in /etc/master.passwd; Procursus ships it locked).

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
# Modeled on the cache's own inetd job com.apple.ftp-proxy-embedded: every key
# exists in Apple's cache. SessionCreate (absent there) put the instances in
# user/501; UserName alone is honoured.
JOB = {
    'Label': 'com.openssh.sshd',
    'Program': '/var/jb/bin/sh',
    'ProgramArguments': ['/var/jb/bin/sh', '/var/jb/usr/libexec/sshd-keygen-wrapper', '-i'],
    'Sockets': {'Listeners': {'SockServiceName': '22', 'SockType': 'stream'}},
    'inetdCompatibility': {'Wait': False},
    'UserName': 'root',
    'GroupName': 'wheel',
    'POSIXSpawnType': 'Interactive',
    'ProcessType': 'Interactive',
}


def main(bootstrap, pubkey):
    pull.untar(bootstrap)
    here_wrapper = os.path.join(here, 'sshd-keygen-wrapper.sh')
    pull.put(open(here_wrapper, 'rb').read(), '/var/jb/usr/libexec/sshd-keygen-wrapper', 0o755)
    mp = os.path.join(here, '..', 'research', 'master.passwd.phone')
    pull.get('/var/jb/etc/master.passwd', mp)
    lines = open(mp).read().splitlines(True)
    lines = [':'.join([l.split(':')[0], '/smx7MYTQIi2M'] + l.split(':')[2:]) if l.startswith('root:') else l
             for l in lines]
    pull.put(''.join(lines).encode(), '/var/jb/etc/master.passwd', 0o600)
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
