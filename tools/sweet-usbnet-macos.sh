#!/bin/sh
# macOS host side of the phone's USB Ethernet (kernel sweetusb=2, Spacewar 0091).
#   tools/sweet-usbnet-macos.sh [LOGFILE]
# Waits for the CDC-ECM interface the phone presents, gives it 192.168.7.1/24
# (the phone is 192.168.7.2), then prints the live kernel console the phone
# streams to UDP port 5140 (appends to LOGFILE if given). Needs sudo for ifconfig.
#
# macOS enumerates a CDC-ECM/NCM gadget as an "AppleUSBECM"/"USB 10/100 LAN"
# interface (enN). It is matched here by the Ethernet address the gadget uses
# for the host side (02:00:5e:77:ee:01); if macOS assigns a different local
# MAC, pass the interface name as $IF=enX instead.
set -eu
host_mac=02:00:5e:77:ee:01
find_if() {
	if [ -n "${IF:-}" ]; then echo "$IF"; return; fi
	# match by the peer (phone) MAC in the ARP/link tables, else by the
	# CDC-ECM service name in networksetup.
	networksetup -listallhardwareports 2>/dev/null \
	  | awk '/USB.*(LAN|ECM|Ethernet)/{getline; print $2; exit}'
}
echo "waiting for the phone's USB Ethernet..." >&2
ifname=""
while [ -z "$ifname" ]; do ifname=$(find_if || true); [ -z "$ifname" ] && sleep 1; done
echo "interface: $ifname" >&2
up_cmd="/sbin/ifconfig $ifname inet 192.168.7.1 netmask 255.255.255.0 up"
if [ -t 0 ]; then
	sudo $up_cmd
	# keep the sudo ticket alive so the address can be re-applied without a prompt
	( while sudo -n -v 2>/dev/null; do sleep 60; done ) &
	reup="sudo -n $up_cmd"
else
	# no terminal (e.g. Claude Code's ! prompt): macOS admin-password dialog instead of sudo
	osascript -e "do shell script \"$up_cmd\" with administrator privileges"
	reup="osascript -e 'do shell script \"$up_cmd\" with administrator privileges'"
fi
echo "$ifname up: 192.168.7.1 <-> phone 192.168.7.2" >&2
# share the Mac's internet to the phone (optional): tools/sweet-nat-macos.sh on.
echo "live kernel log on udp/5140 (ctrl-C to stop)" >&2
# The phone re-enumerates on every reboot and macOS then drops 192.168.7.1 (the
# interface falls back to 169.254.x), and the phone's kernel only streams after
# it hears a hello (patch 0096). So: re-apply the address when it disappears and
# send a hello whenever nothing arrived for a few seconds.
IFNAME="$ifname" REUP="$reup" LOGFILE="${1:-}" exec /usr/bin/python3 - <<'PY'
import os, socket, subprocess, sys, time
ifname, reup = os.environ["IFNAME"], os.environ["REUP"]
logf = open(os.environ["LOGFILE"], "ab") if os.environ.get("LOGFILE") else None

def has_addr():
    out = subprocess.run(["/sbin/ifconfig", ifname], capture_output=True, text=True).stdout
    return "inet 192.168.7.1 " in out

s = None
last = 0.0
while True:
    if not has_addr():
        if s:
            s.close()
            s = None
        print("192.168.7.1 lost on %s, re-applying" % ifname, file=sys.stderr)
        subprocess.run(reup, shell=True)
        time.sleep(1)
        continue
    if s is None:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind(("192.168.7.1", 5140))
            s.settimeout(2)
        except OSError:
            s = None
            time.sleep(1)
            continue
    if time.time() - last > 5:
        try:
            s.sendto(b"hello", ("192.168.7.2", 5140))
        except OSError:
            pass
    try:
        data, _ = s.recvfrom(65535)
    except socket.timeout:
        continue
    except OSError:
        s.close()
        s = None
        continue
    last = time.time()
    sys.stdout.buffer.write(data); sys.stdout.buffer.flush()
    if logf:
        logf.write(data); logf.flush()
PY
