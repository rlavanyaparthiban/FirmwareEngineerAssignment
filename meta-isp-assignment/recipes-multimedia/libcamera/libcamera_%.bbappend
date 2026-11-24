FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# Fixes for the virtual pipeline handler, backported from libcamera 0.5.0.
# In 0.4.0 the handler never fills in FrameMetadata, so every completed buffer
# reports an uninitialised status and sequence number. See REPORT.md.
SRC_URI += " \
    file://0001-libcamera-virtual-Query-number-of-planes-correctly.patch \
    file://0002-libcamera-pipeline-virtual-Simplify-error-return.patch \
    file://0003-libcamera-pipeline-virtual-Fill-buffer-s-metadata.patch \
    file://0004-libcamera-pipeline-virtual-Set-FrameError-on-error.patch \
"

# The virtual pipeline handler (selected for QEMU machines through
# LIBCAMERA_PIPELINES in the distro configuration) needs libyuv and libjpeg.
# Without a libyuv pkg-config file libcamera's meson build would try to
# download libyuv at configure time, which bitbake rightly forbids.
DEPENDS:append:qemuall = " libyuv jpeg"
