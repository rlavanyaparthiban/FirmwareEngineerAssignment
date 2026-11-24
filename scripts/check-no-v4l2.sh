#!/bin/sh
# Constraint check: the application must not use V4L2 from userspace.
#
# Fails if the application sources include <linux/videodev2.h>, issue any
# VIDIOC_* ioctl, shell out to v4l2-ctl, or mention V4L2 at all. Kernel
# drivers and libcamera itself use V4L2 internally; that is allowed and is not
# what this script scans.
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
APP_DIR="${1:-$ROOT/meta-isp-assignment/recipes-isp/isp-channel/files}"

if [ ! -d "$APP_DIR" ]; then
    echo "check-no-v4l2: application directory not found: $APP_DIR" >&2
    exit 2
fi

PATTERN='videodev2\.h|VIDIOC_|v4l2|V4L2|/dev/video|media-ctl'

if grep -rnE "$PATTERN" "$APP_DIR"; then
    echo "check-no-v4l2: FAIL - V4L2 usage found in $APP_DIR" >&2
    exit 1
fi

echo "check-no-v4l2: PASS - no V4L2 usage in $APP_DIR"
