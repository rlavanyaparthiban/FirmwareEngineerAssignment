SUMMARY = "YUV scaling and colour-space conversion library"
DESCRIPTION = "libyuv is required by libcamera's virtual pipeline handler, \
which uses it to convert test patterns and JPEG test frames to NV12."
HOMEPAGE = "https://chromium.googlesource.com/libyuv/libyuv/"
SECTION = "libs"
LICENSE = "BSD-3-Clause"
LIC_FILES_CHKSUM = "file://LICENSE;md5=464282cfb405b005b9637f11103a7325"

DEPENDS = "jpeg"

# Same revision that libcamera 0.4.0 pins in subprojects/libyuv.wrap.
SRC_URI = "git://chromium.googlesource.com/libyuv/libyuv;protocol=https;branch=main \
           file://libyuv.pc.in"
SRCREV = "93b1b332cd60b56ab90aea14182755e379c28a80"
PV = "1770+git"

S = "${WORKDIR}/git"

inherit cmake pkgconfig

# libcamera links the static archive into libcamera.so, so it must be PIC.
EXTRA_OECMAKE = "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"

# Upstream installs an unversioned shared library and a demo tool, and ships
# no pkg-config file. Keep only the static archive and headers, and add the
# libyuv.pc that libcamera's meson build looks for.
do_install() {
    install -d ${D}${libdir}/pkgconfig ${D}${includedir}
    install -m 0644 ${B}/libyuv.a ${D}${libdir}/
    cp -R --no-dereference --preserve=mode,links ${S}/include/* ${D}${includedir}/
    sed -e 's|@PREFIX@|${prefix}|' \
        -e 's|@LIBDIR@|${libdir}|' \
        -e 's|@INCLUDEDIR@|${includedir}|' \
        -e 's|@VERSION@|${PV}|' \
        ${WORKDIR}/libyuv.pc.in > ${D}${libdir}/pkgconfig/libyuv.pc
}

# Static-only library: the main package is empty.
ALLOW_EMPTY:${PN} = "1"
