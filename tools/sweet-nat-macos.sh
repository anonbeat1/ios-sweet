#!/bin/sh
# Share the Mac's internet with the phone over its USB Ethernet (phone 192.168.7.2,
# Mac 192.168.7.1, see tools/sweet-usbnet-macos.sh).
#   tools/sweet-nat-macos.sh [on|off]
# on : IP forwarding + pf NAT of 192.168.7.0/24 to the default-route interface, loaded
#      into the anchor com.apple/ios-sweet (macOS' /etc/pf.conf already evaluates
#      "com.apple/*", so pf.conf is not edited).
# off: flush that anchor and turn IP forwarding off.
# Needs admin rights: sudo in a terminal, the macOS password dialog otherwise.
set -eu
mode=${1:-on}
uplink=$(route -n get default 2>/dev/null | awk '/interface:/{print $2}')
if [ "$mode" = on ]; then
	[ -n "$uplink" ] || { echo "no default route on the Mac" >&2; exit 1; }
	cmd="sysctl -w net.inet.ip.forwarding=1 && echo 'nat on $uplink from 192.168.7.0/24 to any -> ($uplink)' | pfctl -a com.apple/ios-sweet -f - && (pfctl -E 2>&1 | grep -v ALTQ || true)"
else
	cmd="pfctl -a com.apple/ios-sweet -F all; sysctl -w net.inet.ip.forwarding=0"
fi
if [ -t 0 ]; then
	sudo sh -c "$cmd"
else
	osascript -e "do shell script \"$cmd\" with administrator privileges"
fi
echo "NAT $mode (uplink ${uplink:-?}); check: pfctl -a com.apple/ios-sweet -s nat" >&2
