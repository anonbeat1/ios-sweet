#!/bin/bash
# macOS port of mkiosroot.sh: same volume layout and SWEET additions (see the
# header there), built with the system's own tools instead of apfs-fuse and
# hfsprogs.
#
#   sudo mkiosroot-macos.sh OS.dmg SYSTEMOS.dmg APPOS.dmg OUT.img [SIZE]   (SIZE default 16g)
#
# hdiutil mounts the IPSW's APFS images read-only (reads decompress
# LZBITMAP files), newfs_hfs makes the case-sensitive journaled HFS+ volume
# directly in a raw image (no partition map: XNU mounts the userdata
# partition as the volume), and rsync -aH copies without xattrs or
# compression, like the Linux path. OUT.img can be flashed raw to userdata.
set -euo pipefail

[[ $# -ge 4 ]] || { sed -n '2,13p' "$0"; exit 2; }
[[ $EUID == 0 ]] || { echo "run with sudo (owners, modes and hard links)" >&2; exit 1; }
os=$(realpath "$1") sysos=$(realpath "$2") appos=$(realpath "$3") out=$4 size=${5:-16g}
[[ ! -e $out ]] || { echo "refusing to overwrite $out" >&2; exit 1; }

work=$(mktemp -d)
devs=()
cleanup() {
	for d in ${devs[@]+"${devs[@]}"}; do
		hdiutil detach -force "$d" >/dev/null 2>&1 || true
	done
	rmdir "$work"/* "$work" 2>/dev/null || true
}
trap cleanup EXIT

# The OS image is UDIF, the cryptexes are raw APFS (container superblock magic at 0x20).
raw_apfs() { [[ $(dd if="$1" bs=1 skip=32 count=4 2>/dev/null) == NXSB ]]; }
mount_any() {
	mkdir "$work/$2"
	local cls=()
	raw_apfs "$1" && cls=(-imagekey diskimage-class=CRawDiskImage)
	# bash 3.2 (macOS) + set -u: an empty array must be expanded this way
	devs+=("$(hdiutil attach -readonly -nobrowse -noverify -owners on ${cls[@]+"${cls[@]}"} -mountpoint "$work/$2" "$1" | awk 'NR==1{print $1}')")
	[[ -d $work/$2/System || -d $work/$2/usr ]] || { echo "$1: nothing mounted at $work/$2" >&2; exit 1; }
}
mount_any "$os" os
mount_any "$sysos" sysos
mount_any "$appos" appos

mkfile -n "$size" "$out"
dev=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$out" | awk 'NR==1{print $1}')
devs=("$dev" "${devs[@]}")
newfs_hfs -s -J -v iOSRoot "$dev"
mkdir "$work/root"
mount -t hfs "$dev" "$work/root"
root=$work/root

copy() {
	echo "copying $1 -> ${2#"$root"}"
	mkdir -p "$2"
	rsync -aH --numeric-ids "$1/" "$2/"
}
copy "$work/os" "$root"
copy "$work/sysos" "$root/private/preboot/Cryptexes/OS"
copy "$work/appos" "$root/private/preboot/Cryptexes/App"

# Shared cache where dyld looks at ignition level 1: hard links, no extra space.
dsc=private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld
mkdir -p "$root/System/Library/Caches/com.apple.dyld"
for f in "$root/$dsc"/*; do
	ln "$f" "$root/System/Library/Caches/com.apple.dyld/${f##*/}"
done

# Cryptex files without a counterpart in the OS root, hard-linked to their root path.
for c in OS App; do
	(cd "$root/private/preboot/Cryptexes/$c" && find . ! -type d ! -path './System/Library/Caches/com.apple.dyld/*') |
	while IFS= read -r f; do
		f=${f#./}
		if [[ ! -e $root/$f && ! -L $root/$f ]]; then
			mkdir -p "$root/$(dirname "$f")"
			src="$root/private/preboot/Cryptexes/$c/$f"
			if [[ -L $src ]]; then cp -P "$src" "$root/$f"; else ln "$src" "$root/$f"; fi
			echo "linked $c:/$f"
		fi
	done
done

# logd's log stores (owner _logd 272, .migrated markers).
db="$root/private/var/db"
install -d -o 272 -g 272 -m 0751 "$db/diagnostics"
install -d -o 272 -g 272 -m 0755 "$db/uuidtext" "$db/diagnostics/timesync"
for d in diagnostics uuidtext diagnostics/timesync; do
	install -o 272 -g 272 -m 0644 /dev/null "$db/$d/.migrated"
done

# System keybag for the built-in AppleKeyStore (no passcode).
install -d -m 0700 -o 0 -g 0 "$root/private/var/keybags"
printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
	'<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	'<plist version="1.0"><dict><key>KeyBagKeys</key><data>U1dFRVQ=</data></dict></plist>' \
	>"$root/private/var/keybags/systembag.kb"
chown 0:0 "$root/private/var/keybags/systembag.kb"
chmod 0600 "$root/private/var/keybags/systembag.kb"

# Bring-up changes (Setup skipped, activation seeded, crash loops disabled...).
python3 "$(dirname "$0")/sweet-root-tweaks.py" "$root"

df -h "$root"
umount "$root"
fsck_hfs -f -y "$dev" || true
fsck_hfs -f -n "$dev"
