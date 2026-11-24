#!/usr/bin/env python3
"""Regenerate docs/architecture.svg (the one-page architecture diagram)."""
import os

W, H = 1280, 900
out = []


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def rect(x, y, w, h, fill, stroke="#334155", dash=False):
    d = ' stroke-dasharray="6 4"' if dash else ""
    out.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="8" fill="{fill}" '
               f'stroke="{stroke}" stroke-width="1.5"{d}/>')


def text(x, y, s, size=13, weight="normal", fill="#0f172a", anchor="start"):
    out.append(f'<text x="{x}" y="{y}" font-size="{size}" font-weight="{weight}" fill="{fill}" '
               f'text-anchor="{anchor}">{esc(s)}</text>')


def lines(x, y, items, size=12, gap=17, fill="#1e293b"):
    for i, s in enumerate(items):
        text(x, y + i * gap, s, size, fill=fill)


def arrow(x1, y1, x2, y2):
    out.append(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="#0f172a" stroke-width="2" '
               'marker-end="url(#a)"/>')


LX, LW = 150, 720   # main column
RX, RW = 900, 360   # "on a Raspberry Pi" column


def layer(y, h, name, sub, fill):
    text(20, y + 22, name, 13, "bold")
    for i, s in enumerate(sub):
        text(20, y + 40 + i * 15, s, 11, fill="#64748b")
    rect(LX, y, LW, h, fill)


def pi(y, h, items):
    rect(RX, y, RW, h, "#f8fafc", "#94a3b8", dash=True)
    lines(RX + 12, y + 20, items, 11.5, 16, "#334155")


text(20, 30, "Camera ISP channel: architecture (Track Q, qemuarm64)", 19, "bold")
text(RX, 30, "Same layer on a Raspberry Pi 4", 14, "bold", "#475569")
text(RX, 46, "what replaces each block on real hardware", 11, fill="#64748b")

# Sensor
layer(60, 62, "Sensor", ["virtual"], "#fef3c7")
text(LX + 14, 84, "Test-chart frames: 8 x 1920x1080 JPEG, played in a loop", 13, "bold")
lines(LX + 14, 103, ["/usr/share/isp-channel/frames/  +  virtual.yaml (camera 'VirtualSensor0', 1080p30)"])
pi(60, 62, ["IMX219 / IMX477 / IMX708 image sensor", "RAW10/12 Bayer over MIPI CSI-2"])

# Kernel
layer(146, 62, "Kernel", ["linux-yocto 6.6"], "#e0e7ff")
text(LX + 14, 170, "DMA-BUF system heap (fallback: udmabuf)", 13, "bold")
lines(LX + 14, 189, ["frame-buffer memory only, enabled by a kernel config fragment; no capture driver"])
pi(146, 62, ["Unicam CSI-2 receiver, bcm2835-isp (hardware ISP),", "bcm2835-codec (H.264). These drivers are V4L2",
             "inside the kernel; userspace still only sees libcamera."])

# libcamera
layer(232, 80, "libcamera", ["0.4.0", "pipeline handler"], "#dcfce7")
text(LX + 14, 256, "CameraManager  ->  PipelineHandlerVirtual", 13, "bold")
lines(LX + 14, 275, ["decodes the JPEGs, scales them with libyuv, copies one NV12 frame into a",
                     "DMABUF FrameBuffer per queued Request. One NV12 stream, no RAW, no IPA, no pacing."])
pi(232, 80, ["rpi/vc4 pipeline handler + Raspberry Pi IPA module", "+ per-sensor tuning file: AE, AWB, lens shading,",
             "CCM and gamma run in the IPA and the hardware ISP.", "Viewfinder YUV420 and RAW Bayer streams together."])

# Application
AY, AH = 340, 330
layer(AY, AH, "Application", ["isp-channel", "libcamera C++ API", "only (no V4L2)"], "#fff7ed")
bw, gap, by, bh = 146, 36, AY + 24, 146
bx = [LX + 14 + i * (bw + gap) for i in range(4)]
titles = ["CameraSource", "SensorModel", "SoftIsp", "Encoder"]
body = [
    ["enumerate, configure", "4 recycled Requests", "30 fps frame clock", "DMABUF mmap,", "read-only: zero copy", "control routing"],
    ["the 'mosaic step'", "NV12 -> linear light", "-> inverse CCM", "-> illuminant cast", "-> exposure, pedestal", "-> RAW10 RGGB"],
    ["1 black level", "2 white balance", "3 bilinear demosaic", "4 colour matrix", "5 gamma, bright/contr.", "6 RGB -> I420 (BT.709)"],
    ["GStreamer appsrc", "ISP writes straight", "into the GstBuffer", "(no copy)", "back-pressure when", "the encoder is busy"],
]
fills = ["#ffffff", "#fde68a", "#fdba74", "#ffffff"]
for i in range(4):
    rect(bx[i], by, bw, bh, fills[i])
    text(bx[i] + bw / 2, by + 20, titles[i], 13, "bold", anchor="middle")
    lines(bx[i] + 9, by + 42, body[i], 11, 17)
for i, label in enumerate(["NV12", "RAW Bayer", "I420"]):
    arrow(bx[i] + bw, by + 78, bx[i + 1] - 2, by + 78)
    text((bx[i] + bw + bx[i + 1]) / 2, by - 6, label, 10.5, fill="#475569", anchor="middle")
text(bx[2] + bw / 2, by + bh + 15, "row bands on N threads", 10.5, fill="#9a3412", anchor="middle")

sy, sh, sw = AY + 196, 118, 160
sx = [LX + 14 + i * (sw + 6) for i in range(4)]
stitles = ["IspController", "Control inputs", "SnapshotWriter", "FrameStats"]
sbody = [
    ["gray-world AWB, presets", "AE loop (mean luma)", "drives SoftIsp and the", "sensor exposure / gain"],
    ["UNIX socket + isp-ctl:", "set / get / snapshot", "SIGUSR1: snapshot", "SIGTERM: clean stop"],
    ["PNG of the ISP output", "own thread (libpng)", "self-test: PSNR vs", "the source picture"],
    ["FPS, per-stage ms,", "CPU %, RSS, drops", "one line per interval", "-> journald"],
]
for i in range(4):
    rect(sx[i], sy, sw, sh, "#ffffff", "#94a3b8")
    text(sx[i] + sw / 2, sy + 19, stitles[i], 12.5, "bold", anchor="middle")
    lines(sx[i] + 8, sy + 40, sbody[i], 11, 16)
pi(AY, AH, ["The RAW stream comes from the sensor, so the", "mosaic step is not used: CameraSource hands the",
            "camera's Bayer buffers to SoftIsp directly.", "",
            "The hardware ISP already produces the YUV", "viewfinder; the software ISP then becomes the",
            "'own RAW stages' comparison of Track H.", "",
            "Controls are sent to the camera (it lists them", "in its ControlInfoMap) instead of being",
            "implemented in software.", "",
            "The sensor paces the frames: frame-pacing = off."])

# Encode + sink
layer(684, 74, "Encode + sink", ["GStreamer 1.22"], "#fce7f3")
text(LX + 14, 708, "x264enc (software H.264)  ->  splitmuxsink / mp4mux  ->  /var/lib/isp-channel/isp-NNNNN.mp4", 13, "bold")
lines(LX + 14, 727, ["ten one-minute MP4 segments kept as a ring. The pipeline is a configuration string:",
                     "MJPEG (jpegenc) or RTP over UDP (rtph264pay ! udpsink) need no code change."])
pi(684, 74, ["Pi 4: hardware H.264 encoder (bcm2835-codec)", "selected in the same pipeline string.",
             "Pi 5: no hardware encoder, software H.264 as here."])

# System
layer(772, 62, "System", ["Yocto scarthgap"], "#f1f5f9")
text(LX + 14, 796, "systemd: isp-channel.service (unprivileged user 'ispchannel', starts at boot)", 13, "bold")
lines(LX + 14, 815, ["udev rule opens the DMABUF allocators to group 'video'; journald; dropbear SSH; networkd DHCP"])
pi(772, 62, ["Same unit and image recipe. Boot firmware:", "camera_auto_detect=1, gpu_mem=76, cma=256M"])

# Data flow between the layers (kept clear of the text on the left).
jx, dx = LX + LW - 40, LX + LW - 170
arrow(jx, 122, jx, 232)
text(jx - 8, 141, "JPEG files", 11, fill="#475569", anchor="end")
arrow(dx, 208, dx, 232)
text(dx - 8, 224, "DMABUF allocation", 11, fill="#475569", anchor="end")
cx = bx[0] + bw / 2
arrow(cx, 312, cx, by - 1)
text(cx + 10, 330, "completed Request: FrameBuffer (DMABUF fd)", 11, fill="#475569")
ex = LX + LW - 20
arrow(ex, by + bh, ex, 684)
text(ex - 8, by + bh + 15, "I420 frames", 11, fill="#475569", anchor="end")

text(20, 866, "Copies on the frame path: (1) libcamera fills the DMABUF  (2) the mosaic step reads it in place and writes RAW  "
     "(3) the ISP writes I420 into the encoder buffer  (4) x264 keeps its own reference frames.", 11.5, fill="#334155")
text(20, 884, "Nothing else is copied; a snapshot adds one RGB888 buffer only for the frame being saved.", 11.5, fill="#334155")

svg = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}" '
       'font-family="Helvetica, Arial, sans-serif">\n'
       '<defs><marker id="a" markerWidth="10" markerHeight="8" refX="9" refY="4" orient="auto">'
       '<path d="M0,0 L10,4 L0,8 z" fill="#0f172a"/></marker></defs>\n'
       f'<rect width="{W}" height="{H}" fill="#ffffff"/>\n' + "\n".join(out) + "\n</svg>\n")
path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "docs", "architecture.svg")
with open(path, "w", encoding="utf-8") as f:
    f.write(svg)
print(path)
