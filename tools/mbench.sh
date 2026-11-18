#!/bin/sh
# Time the 3D view on a MiSTer: build the BENCH ROM (the same 256 frames
# every run), put it on the MiSTer, start it, and bring back a screenshot
# with the yellow line of results on it. Then put the game's ROM back in
# rom/.
#   tools/mbench.sh [out.png] [extra make arguments, e.g. RES=3]
# MISTER names the machine (default root@mister.tv.local), SEED the dungeon.
set -e
OUT=${1:-shots/mbench.png}
[ $# -gt 0 ] && shift
MISTER=${MISTER:-root@mister.tv.local}
SEED=${SEED:-7}

make BENCH=$SEED "$@" deploy MISTER=$MISTER >/dev/null
SHOT=$(ssh $MISTER '
	echo "<mistergamedescription><rbf>_Console/S32X</rbf><file delay=\"2\" type=\"f\" index=\"0\" path=\"voxelike.32x\"/></mistergamedescription>" > /tmp/vox.mgl
	echo "load_core /tmp/vox.mgl" > /dev/MiSTer_cmd
	sleep ${WAIT:-50}
	echo screenshot > /dev/MiSTer_cmd
	sleep 3
	ls -t /media/fat/screenshots/S32X | head -1
	echo "load_core /media/fat/menu.rbf" > /dev/MiSTer_cmd')
mkdir -p "$(dirname "$OUT")"
scp -q "$MISTER:/media/fat/screenshots/S32X/$SHOT" "$OUT"
make "$@" >/dev/null
echo "$OUT"
