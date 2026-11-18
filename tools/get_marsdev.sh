#!/bin/sh
# Fetch a prebuilt Marsdev toolchain for Linux x86_64 and put it where
# MARSDEV will point: tools/get_marsdev.sh v1.0.0-rc1 /path/to/marsdev
set -eu
VER=$1
DEST=$2
TMP=$(mktemp -d)
curl -fsSL "https://github.com/andwn/marsdev/releases/download/$VER/marsdev-$VER-linux-x86_64.tar.xz" | tar -xJ -C "$TMP"
GCC=$(find "$TMP" -path '*/sh-elf/bin/sh-elf-gcc' | head -1)
[ -n "$GCC" ] || { echo "no sh-elf-gcc in the Marsdev archive" >&2; exit 1; }
mkdir -p "$DEST"
cp -a "$(dirname "$(dirname "$(dirname "$GCC")")")"/. "$DEST"/
rm -rf "$TMP"
