#!/var/jb/bin/sh
SSHDIR=/var/jb/etc/ssh

# SWEET: the Procursus postinst steps sshd needs (no dpkg run on this device):
# pwd.db for libiosexec's getpwnam, /var/jb/etc/shells, root's authorized_keys.
{
	[ -f /var/jb/etc/pwd.db ] || /var/jb/usr/sbin/pwd_mkdb -p /var/jb/etc/master.passwd
	[ -f /var/jb/etc/shells ] || /var/jb/bin/cp -p /var/jb/usr/share/debianutils/shells /var/jb/etc/shells
	if [ ! -f /var/jb/var/root/.ssh/authorized_keys ]; then
		/var/jb/bin/mkdir -p /var/jb/var/root/.ssh
		/var/jb/bin/cp /var/root/.ssh/authorized_keys /var/jb/var/root/.ssh/authorized_keys
		/var/jb/usr/bin/chmod 700 /var/jb/var/root/.ssh
		/var/jb/usr/bin/chmod 600 /var/jb/var/root/.ssh/authorized_keys
	fi
} >> /var/jb/tmp/sweet-sshd-setup.log 2>&1

[ ! -f ${SSHDIR}/ssh_host_rsa_key ] && /var/jb/usr/bin/ssh-keygen -q -t rsa  -f ${SSHDIR}/ssh_host_rsa_key -N "" -C "" < /dev/null > /dev/null 2> /dev/null
[ ! -f ${SSHDIR}/ssh_host_ecdsa_key ] && /var/jb/usr/bin/ssh-keygen -q -t ecdsa  -f ${SSHDIR}/ssh_host_ecdsa_key -N "" -C "" < /dev/null > /dev/null 2> /dev/null
[ ! -f ${SSHDIR}/ssh_host_ed25519_key ] && /var/jb/usr/bin/ssh-keygen -q -t ed25519  -f ${SSHDIR}/ssh_host_ed25519_key -N "" -C "" < /dev/null > /dev/null 2> /dev/null

exec /var/jb/usr/sbin/sshd -E /var/jb/tmp/sshd.log $@
