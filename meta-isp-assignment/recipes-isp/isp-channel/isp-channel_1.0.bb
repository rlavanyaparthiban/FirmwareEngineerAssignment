SUMMARY = "Camera ISP channel application"
DESCRIPTION = "Captures frames through the libcamera C++ API, runs a software \
ISP (black level, gray-world AWB, bilinear demosaic, CCM, gamma) on RAW Bayer \
data and encodes the result to H.264 with GStreamer. Started at boot by a \
systemd unit."
SECTION = "multimedia"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

DEPENDS = "libcamera gstreamer1.0 gstreamer1.0-plugins-base libpng"

SRC_URI = "file://isp-channel \
           file://isp-channel.service \
           file://isp-channel.conf \
           file://60-isp-channel.rules"

S = "${WORKDIR}/isp-channel"

inherit cmake pkgconfig systemd useradd

# The unit tests run on the build host in CI, not on the target.
EXTRA_OECMAKE = "-DBUILD_TESTING=OFF"

do_install:append() {
    install -D -m 0644 ${WORKDIR}/isp-channel.service \
        ${D}${systemd_system_unitdir}/isp-channel.service
    install -D -m 0644 ${WORKDIR}/isp-channel.conf \
        ${D}${sysconfdir}/isp-channel/isp-channel.conf
    install -D -m 0644 ${WORKDIR}/60-isp-channel.rules \
        ${D}${nonarch_base_libdir}/udev/rules.d/60-isp-channel.rules
}

SYSTEMD_SERVICE:${PN} = "isp-channel.service"
SYSTEMD_AUTO_ENABLE = "enable"

# The service runs as an unprivileged user. Membership of "video" gives it the
# DMABUF allocator devices (see 60-isp-channel.rules) and, on real hardware,
# the camera nodes libcamera opens.
USERADD_PACKAGES = "${PN}"
USERADD_PARAM:${PN} = "--system --home-dir ${localstatedir}/lib/isp-channel --no-create-home \
                       --shell /sbin/nologin --groups video --user-group ispchannel"

FILES:${PN} += "${nonarch_base_libdir}/udev/rules.d"
CONFFILES:${PN} = "${sysconfdir}/isp-channel/isp-channel.conf"

# GStreamer elements named in the default pipeline: appsrc, x264enc,
# splitmuxsink (mp4mux inside), plus the documented alternatives (MJPEG, RTP
# over UDP).
RDEPENDS:${PN} = " \
    gstreamer1.0-plugins-base-app \
    gstreamer1.0-plugins-good-isomp4 \
    gstreamer1.0-plugins-good-multifile \
    gstreamer1.0-plugins-good-jpeg \
    gstreamer1.0-plugins-good-matroska \
    gstreamer1.0-plugins-good-rtp \
    gstreamer1.0-plugins-good-udp \
    gstreamer1.0-plugins-ugly-x264 \
"
# On QEMU the camera is libcamera's virtual pipeline handler.
RDEPENDS:${PN}:append:qemuall = " virtual-camera-config"

# The configuration file and the camera dependency differ per machine.
PACKAGE_ARCH = "${MACHINE_ARCH}"
