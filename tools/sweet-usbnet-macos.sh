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
if [ -t 0 ]; then
	sudo ifconfig "$ifname" inet 192.168.7.1 netmask 255.255.255.0 up
else
	# no terminal (e.g. Claude Code's ! prompt): macOS admin-password dialog instead of sudo
	osascript -e "do shell script \"/sbin/ifconfig $ifname inet 192.168.7.1 netmask 255.255.255.0 up\" with administrator privileges"
fi
echo "$ifname up: 192.168.7.1 <-> phone 192.168.7.2" >&2
# share the Mac's internet to the phone (optional): enable IP forwarding + a
# pf NAT rule on the uplink. Left to the user; link-local works without it.
echo "live kernel log on udp/5140 (ctrl-C to stop)" >&2
# BSD nc's UDP listener binds to the first peer; use a plain UDP socket.
LOGFILE="${1:-}" exec /usr/bin/python3 - <<'PY'
import os, socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("192.168.7.1", 5140))
logf = open(os.environ["LOGFILE"], "ab") if os.environ.get("LOGFILE") else None
while True:
    data, _ = s.recvfrom(65535)
    sys.stdout.buffer.write(data); sys.stdout.buffer.flush()
    if logf:
        logf.write(data); logf.flush()
PY
