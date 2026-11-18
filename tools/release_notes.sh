#!/bin/sh
# A release's notes: what was committed since the release before it.
#   tools/release_notes.sh 0.1.0-alpha.3 [v0.1.0-alpha.2] [rom]
set -eu
V=$1
PREV=${2:-}
ROM=${3:-}
echo "## Voxelike32X $V"
echo
echo "Alpha: Rogue 5.4.4 in voxels on the Sega 32X. Things are missing and things will change."
echo
echo "### Changes"
if [ -n "$PREV" ]; then
	git log --no-merges --pretty='- %s' "$PREV..HEAD"
else
	echo "- The first alpha build."
fi
echo
echo "### Playing it"
echo "Load \`voxelike-$V.32x\` in a 32X emulator, a MiSTer's S32X core, or from a flash cart. The title screen says $V. The controls are in the README."
if [ -n "$ROM" ]; then
	echo
	echo "SHA-256: \`$(sha256sum "$ROM" | cut -d' ' -f1)\`"
fi
echo
echo "Rogue is copyright its authors and distributed under its own licence, attached as \`LICENSE-rogue.txt\`. The rest is under \`LICENSE-voxelike.txt\`."
