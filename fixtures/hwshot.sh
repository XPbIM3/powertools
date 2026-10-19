#!/bin/sh
# hwshot.sh -- collect a full IOKit registry snapshot ("hardware shot") from
# this machine into ONE archive named after the hardware, e.g.:
#
#     hwshot-PowerBook7,3-10.5.8.txt.gz
#
# so we always know which iron a given dump came from. The dump is written
# into the same directory the script itself lives in ("under its feet").
#
# Scope: all known ioreg planes + system identity header. Read-only, no sudo,
# installs nothing, loads nothing. Pure observation. ASCII only, /bin/sh.

# --- where am I? write the dump next to the script itself ---
case "$0" in
    */*) DIR=`dirname "$0"` ;;
    *)   DIR=. ;;
esac
DIR=`cd "$DIR" && pwd`

# --- identity: hardware model + OS version -> filename ---
MODEL=`sysctl -n hw.model 2>/dev/null`
[ -z "$MODEL" ] && MODEL=unknown-model
OSVER=`sw_vers -productVersion 2>/dev/null`
[ -z "$OSVER" ] && OSVER=unknown-OS

BASE="hwshot-${MODEL}-${OSVER}"
H="$DIR/$BASE.txt"

# never clobber a previous shot of the same iron: add -1, -2, ... if needed
n=1
while [ -f "$H" ] || [ -f "$H.gz" ]; do
    H="$DIR/$BASE-$n.txt"
    n=`expr $n + 1`
done

# --- header: identity block, cheap first ---
echo "=== hwshot `date` ===" > "$H"
echo "host:   `hostname 2>/dev/null`" >> "$H"
echo "user:   `whoami 2>/dev/null`" >> "$H"
echo "model:  $MODEL" >> "$H"
echo "os:     $OSVER" >> "$H"
sw_vers >> "$H" 2>&1
echo "--- sysctl ---" >> "$H"
sysctl hw.model hw.ncpu hw.cputype hw.cpusubtype hw.byteorder \
       hw.memsize 2>/dev/null >> "$H"

# --- the dump itself: every plane known to be interesting on darwin8/9 ---
# unknown planes are harmless: their error line lands in the file, loop goes on
for p in IOService IODeviceTree IOUSB IOPCIDevice IOFireWire \
         IOMedia IOResources; do
    echo "=== ioreg plane $p ===" >> "$H"
    ioreg -p "$p" -l -w 0 >> "$H" 2>&1
done

# --- pack it (text of a full dump compresses ~10x); keep .txt if gzip fails
if gzip -9 -f "$H" 2>/dev/null; then
    F="$H.gz"
else
    F="$H"
fi

echo "done: $F"
ls -l "$F"
