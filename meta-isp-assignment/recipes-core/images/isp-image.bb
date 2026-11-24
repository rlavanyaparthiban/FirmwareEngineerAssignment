SUMMARY = "Camera ISP channel image"
DESCRIPTION = "core-image-minimal plus libcamera, the isp-channel application \
and its systemd unit, an SSH server and DHCP networking."

require recipes-core/images/core-image-minimal.bb

# Development conveniences: SSH server and a passwordless root login.
# Not suitable for a production image.
IMAGE_FEATURES += "ssh-server-dropbear allow-empty-password empty-root-password allow-root-login"

IMAGE_INSTALL:append = " \
    libcamera \
    isp-channel \
    systemd-analyze \
"

# Room for the rotating recording segments and snapshots.
IMAGE_ROOTFS_EXTRA_SPACE = "524288"

# QEMU: four cores so the multi-threaded ISP has something to scale on, and
# 1 GiB of RAM for the 1080p frame buffers, x264 and the page cache.
QB_SMP = "-smp 4"
QB_MEM = "-m 1024"
