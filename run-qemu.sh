#!/bin/bash
# Boot the isp-image in QEMU (Track Q).
#
#   ./run-qemu.sh [IMAGE_DIR]
#
# IMAGE_DIR holds the kernel "Image" and the "isp-image-qemuarm64.rootfs.ext4"
# root filesystem. It defaults to the kas build output, so after
# `kas build kas/isp-qemuarm64.yml` no argument is needed. Only
# qemu-system-aarch64 is required on the host (Debian/Ubuntu: qemu-system-arm);
# no root, no TAP device.
#
# The serial console is on this terminal (log in as root, no password).
# SSH is forwarded:  ssh -p 2222 root@localhost
# Leave with `poweroff` in the guest, or Ctrl-A X to kill QEMU.
#
# Environment:
#   QEMU_SMP   guest CPUs          (default 4)
#   QEMU_MEM   guest RAM in MiB    (default 1024)
#   SSH_PORT   host port for SSH   (default 2222)
#   QEMU_EXTRA extra QEMU arguments
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
IMAGE_DIR="${1:-$HERE/build/tmp/deploy/images/qemuarm64}"
KERNEL="$IMAGE_DIR/Image"
ROOTFS="$IMAGE_DIR/isp-image-qemuarm64.rootfs.ext4"

for f in "$KERNEL" "$ROOTFS"; do
    [ -e "$f" ] || { echo "run-qemu.sh: missing $f" >&2; exit 1; }
done
command -v qemu-system-aarch64 > /dev/null ||
    { echo "run-qemu.sh: qemu-system-aarch64 not found" >&2; exit 1; }

# -machine virt / -cpu cortex-a57: the generic ARMv8-A board and core that the
#     qemuarm64 machine is built and tuned for (same ARMv8-A + NEON baseline as
#     the Cortex-A72 of a Raspberry Pi 4).
# -smp 4: one host thread per guest CPU (multi-threaded TCG), which is what
#     the ISP's band threads and x264's slice threads scale on. Also the core
#     count of a Raspberry Pi 4/5.
# -m 1024: the channel peaks near 200 MiB resident at 1080p (frame buffers,
#     pre-scaled test frames inside libcamera, x264); 1 GiB leaves ample page
#     cache for the recording and is the smallest Raspberry Pi 4 configuration.
# virtio block/net/rng: paravirtual devices, the cheapest to emulate. The
#     empty romfile skips the PXE option ROM, which a minimal QEMU install lacks.
# shellcheck disable=SC2086
exec qemu-system-aarch64 \
    -machine virt -cpu cortex-a57 \
    -smp "${QEMU_SMP:-4}" -m "${QEMU_MEM:-1024}" \
    -nographic \
    -kernel "$KERNEL" \
    -append "root=/dev/vda rw console=ttyAMA0 ip=dhcp" \
    -drive "if=none,id=hd0,format=raw,file=$ROOTFS" \
    -device virtio-blk-pci,drive=hd0 \
    -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:${SSH_PORT:-2222}-:22" \
    -device virtio-net-pci,netdev=net0,romfile= \
    -device virtio-rng-pci \
    ${QEMU_EXTRA:-}
