#!/bin/bash
# End-to-end test of isp-channel on the build host, against a libcamera that
# was built with the virtual pipeline handler (see .github/workflows/ci.yml).
#
#   scripts/native-smoke-test.sh BUILD_DIR LIBCAMERA_PREFIX [SECONDS]
#
# Runs the full channel (virtual camera -> mosaic -> ISP -> H.264 -> MP4),
# exercises the snapshot triggers and the control socket, and checks the
# results. Needs a DMABUF provider (/dev/dma_heap/system or /dev/udmabuf).
set -euo pipefail

BUILD_DIR="$(realpath "${1:?build directory}")"
PREFIX="$(realpath "${2:?libcamera prefix}")"
SECONDS_TO_RUN="${3:-20}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$(mktemp -d)"
FRAMES="$ROOT/meta-isp-assignment/recipes-isp/virtual-camera/files/frames/"

fail() { echo "FAIL: $*" >&2; sed 's/^/    /' "$OUT/run.log" | tail -40 >&2; exit 1; }

# Point the virtual pipeline handler at the frames in the source tree.
CONF_DIR="$PREFIX/share/libcamera/pipeline/virtual"
mkdir -p "$CONF_DIR"
sed "s|/usr/share/isp-channel/frames/|$FRAMES|" \
    "$ROOT/meta-isp-assignment/recipes-isp/virtual-camera/files/virtual.yaml" > "$CONF_DIR/virtual.yaml"

LIBDIR="$(dirname "$(find "$PREFIX" -name 'libcamera.so' | head -1)")"
export LD_LIBRARY_PATH="$LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LIBCAMERA_LOG_LEVELS='*:WARN'

"$BUILD_DIR/isp-channel" --output "$OUT" --socket "$OUT/ctl.sock" \
    --stats-interval 5 --duration "$SECONDS_TO_RUN" > "$OUT/run.log" 2>&1 &
PID=$!

sleep 5
CTL="$BUILD_DIR/isp-ctl -s $OUT/ctl.sock"
$CTL snapshot                       # trigger 1: control socket
sleep 2
$CTL set Brightness 0.2
$CTL set AwbMode tungsten
sleep 2
kill -USR1 "$PID"                   # trigger 2: signal
$CTL get
$CTL set Brightness 2.5 && fail "out-of-range control value was accepted"

wait "$PID" || fail "isp-channel exited with an error"

echo "---- log ----"
grep -E 'stats |summary |PSNR|snapshot saved|control ' "$OUT/run.log" | sed 's/^<[0-9]>//'
echo "-------------"

SUMMARY="$(grep 'summary ' "$OUT/run.log")" || fail "no summary line"
value() { echo "$SUMMARY" | tr ' ' '\n' | sed -n "s/^$1=//p"; }
FRAMES_DONE="$(value frames)"
[ "$(value dropped)" = 0 ] || fail "dropped frames"
[ "$(value failed)" = 0 ] || fail "failed frames"
[ "$FRAMES_DONE" -ge $((SECONDS_TO_RUN * 5)) ] || fail "only $FRAMES_DONE frames"

[ "$(find "$OUT" -name 'snapshot-*.png' | wc -l)" -eq 2 ] || fail "expected two snapshots"
PSNR="$(grep -m1 'PSNR' "$OUT/run.log" | sed 's/.*PSNR \([0-9.]*\).*/\1/')"
awk -v p="$PSNR" 'BEGIN { exit !(p >= 25.0) }' || fail "ISP round-trip PSNR $PSNR dB is below 25 dB"
grep -q 'control AwbMode .*tungsten' "$OUT/run.log" || fail "control change was not applied"

if command -v ffprobe > /dev/null; then
    ENCODED=0
    for f in "$OUT"/isp-*.mp4; do
        n="$(ffprobe -v error -count_frames -select_streams v:0 \
              -show_entries stream=nb_read_frames -of csv=p=0 "$f")"
        ENCODED=$((ENCODED + n))
    done
    [ "$ENCODED" -eq "$FRAMES_DONE" ] || fail "encoded $ENCODED frames, captured $FRAMES_DONE"
    echo "H.264: $ENCODED frames in $(find "$OUT" -name 'isp-*.mp4' | wc -l) file(s)"
fi

if [ -n "${ARTIFACT_DIR:-}" ]; then
    mkdir -p "$ARTIFACT_DIR"
    cp "$OUT"/run.log "$OUT"/*.png "$OUT"/*.mp4 "$ARTIFACT_DIR"/
fi
echo "PASS: $FRAMES_DONE frames, 0 dropped, round-trip PSNR $PSNR dB"
