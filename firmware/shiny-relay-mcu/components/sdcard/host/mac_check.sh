#!/bin/sh
# mac_check.sh IMAGE: macOS reads a card image the library made: attached read-only and listed (FAT32), or, when macOS
# will not mount it here (exFAT: the user-space driver refuses this session), its own fsck_exfat / fsck_msdos -n
IMG=$1
if OUT=$(hdiutil attach -readonly -imagekey diskimage-class=CRawDiskImage -nobrowse "$IMG" 2>/dev/null); then
  DEV=$(echo "$OUT" | head -1 | awk '{print $1}')
  MNT=$(echo "$OUT" | awk -F'\t' '/\/Volumes\//{print $NF}' | tail -1)
  N=$(find "$MNT" -iname '*.gb' -o -iname '*.gbc' | wc -l | tr -d ' ')
  echo "mac-check: $IMG mounted at $MNT ($(/usr/sbin/diskutil info "$MNT" | awk -F: '/Personality/{gsub(/^ +/,"",$2); print $2}')): $N game files, top: $(ls "$MNT" | tr '\n' ' ')"
  hdiutil detach "$DEV" > /dev/null
  [ "$N" -gt 0 ]; exit $?
fi
DEV=$(hdiutil attach -readonly -nomount -imagekey diskimage-class=CRawDiskImage "$IMG" | head -1 | awk '{print $1}')
T=$(/usr/sbin/diskutil info ${DEV}s1 | awk -F: '/Type \(Bundle\)/{gsub(/^ +/,"",$2); print $2}')
case $T in exfat) R=$(/sbin/fsck_exfat -n ${DEV}s1 2>&1 | tail -1);; *) R=$(/sbin/fsck_msdos -n ${DEV}s1 2>&1 | tail -1);; esac
echo "mac-check: $IMG ($T) not mounted here; macOS fsck: $R"
hdiutil detach "$DEV" > /dev/null
echo "$R" | grep -q "appears to be OK"
