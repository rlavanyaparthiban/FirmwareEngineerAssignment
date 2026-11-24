#!/usr/bin/env python3
"""Record a terminal video of the image booting in QEMU and running the app.

    scripts/record-demo.py [--image-dir DIR] [--out FILE.mp4]

Runs run-qemu.sh inside a detached tmux session, types a fixed demonstration
script into the guest's serial console, captures the terminal twice a second
and renders the captures to an MP4. The two PNG snapshots the channel saved
during the session are appended as the last seconds of the video.

It is a faithful capture of a real session, produced without a desktop: the
commands, their output and the timing are what QEMU did. The boot phase is
played at 4x speed, everything after the login prompt in real time.

Host requirements: tmux, qemu-system-aarch64, ffmpeg, e2fsprogs, Pillow.
"""
import argparse
import glob
import os
import shutil
import subprocess
import tempfile
import time

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOTFS = "isp-image-qemuarm64.rootfs.ext4"
COLS, ROWS = 118, 34
FPS = 2
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
SESSION = "isp-demo"

# (command typed into the guest, seconds to show its output)
DEMO = [
    ("# Camera ISP channel: Yocto image booted in QEMU (qemuarm64)", 3),
    ("systemctl status isp-channel --no-pager | head -n 5", 6),
    ("cam -l", 6),
    ("journalctl -u isp-channel --no-pager -o cat | grep -E 'camera|RAW|ISP|control ' | cut -c1-110 | head -n 22", 12),
    ("clear; isp-ctl get", 8),
    ("isp-ctl snapshot", 6),
    ("isp-ctl set Brightness 0.2; isp-ctl set AwbMode tungsten", 12),
    ("systemctl kill -s USR1 isp-channel", 12),
    ("isp-ctl set Brightness 0; isp-ctl set AwbMode auto", 4),
    ("clear; journalctl -u isp-channel --no-pager -o cat | cut -c1-116 | tail -n 9", 12),
    ("ls -l /var/lib/isp-channel", 8),
    ("systemctl stop isp-channel; journalctl -u isp-channel --no-pager -o cat | cut -c1-116 | tail -n 3", 14),
    ("grep -c . /proc/cpuinfo > /dev/null; uname -srm; grep PRETTY /etc/os-release", 5),
]


def tmux(*args):
    return subprocess.run(["tmux", *args], capture_output=True, text=True).stdout


def screen():
    return tmux("capture-pane", "-p", "-t", SESSION)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--image-dir", default=os.path.join(ROOT, "build/tmp/deploy/images/qemuarm64"))
    parser.add_argument("--out", default=os.path.join(ROOT, "docs/evidence/qemu/demo.mp4"))
    args = parser.parse_args()

    work = tempfile.mkdtemp(prefix="isp-demo-")
    os.symlink(os.path.realpath(os.path.join(args.image_dir, "Image")), os.path.join(work, "Image"))
    rootfs = os.path.join(work, ROOTFS)
    subprocess.check_call(["cp", "--sparse=always",
                           os.path.realpath(os.path.join(args.image_dir, ROOTFS)), rootfs])

    font = ImageFont.truetype(FONT, 15)
    cw, ch = font.getbbox("M")[2], 19
    size = (COLS * cw + 24, ROWS * ch + 52)
    frames = []  # (image path, repeat count)
    frame_dir = os.path.join(work, "frames")
    os.makedirs(frame_dir)

    def capture(caption, repeat=1):
        img = Image.new("RGB", size, (16, 20, 28))
        draw = ImageDraw.Draw(img)
        draw.rectangle([0, 0, size[0], 28], fill=(36, 44, 60))
        draw.text((12, 6), caption, font=font, fill=(148, 197, 253))
        for row, line in enumerate(screen().splitlines()[:ROWS]):
            draw.text((12, 40 + row * ch), line[:COLS], font=font, fill=(220, 226, 235))
        path = os.path.join(frame_dir, f"{len(frames):05d}.png")
        img.save(path)
        frames.append((path, repeat))

    def show(caption, seconds):
        end = time.time() + seconds
        while time.time() < end:
            capture(caption)
            time.sleep(1.0 / FPS)

    tmux("kill-session", "-t", SESSION)
    tmux("new-session", "-d", "-s", SESSION, "-x", str(COLS), "-y", str(ROWS),
         f"{os.path.join(ROOT, 'run-qemu.sh')} {work}")
    try:
        # Boot: capture every 2 s and play each capture for one frame = 4x speed.
        start = time.time()
        while "login:" not in screen():
            if time.time() - start > 900:
                raise SystemExit("guest did not reach the login prompt")
            capture("./run-qemu.sh   (boot, played at 4x speed)")
            time.sleep(2.0)
        boot_seconds = time.time() - start
        show("./run-qemu.sh   (boot finished)", 2)

        tmux("send-keys", "-t", SESSION, "root", "Enter")
        time.sleep(3)
        # Give the service time to produce its first statistics lines.
        tmux("send-keys", "-t", SESSION, "clear", "Enter")
        show("logged in as root on the serial console", 2)
        for command, seconds in DEMO:
            tmux("send-keys", "-t", SESSION, "-l", command)
            tmux("send-keys", "-t", SESSION, "Enter")
            show("isp-channel running in QEMU (real time)", seconds)

        tmux("send-keys", "-t", SESSION, "poweroff", "Enter")
        show("poweroff", 4)
        for _ in range(120):
            if not tmux("has-session", "-t", SESSION) and \
               subprocess.run(["tmux", "has-session", "-t", SESSION], capture_output=True).returncode:
                break
            time.sleep(2)
    finally:
        tmux("kill-session", "-t", SESSION)

    # Append the snapshots the channel saved during the session.
    media = os.path.join(work, "media")
    os.makedirs(media)
    subprocess.run(["debugfs", "-R", f"rdump /var/lib/isp-channel {media}", rootfs], capture_output=True)
    for i, path in enumerate(sorted(glob.glob(os.path.join(media, "isp-channel", "snapshot-*.png")))):
        shot = Image.open(path).convert("RGB")
        scale = min((size[0] - 24) / shot.width, (size[1] - 52) / shot.height)
        shot = shot.resize((int(shot.width * scale), int(shot.height * scale)))
        img = Image.new("RGB", size, (16, 20, 28))
        draw = ImageDraw.Draw(img)
        draw.rectangle([0, 0, size[0], 28], fill=(36, 44, 60))
        caption = ("snapshot 1: ISP output, auto white balance" if i == 0 else
                   "snapshot 2: Brightness +0.2 and AwbMode tungsten applied at run time")
        draw.text((12, 6), caption, font=font, fill=(148, 197, 253))
        img.paste(shot, ((size[0] - shot.width) // 2, 40))
        out = os.path.join(frame_dir, f"{len(frames):05d}.png")
        img.save(out)
        frames.append((out, 6 * FPS))

    # ffmpeg concat list with per-frame durations.
    listing = os.path.join(work, "frames.txt")
    with open(listing, "w") as f:
        for path, repeat in frames:
            f.write(f"file '{path}'\nduration {repeat / FPS}\n")
        f.write(f"file '{frames[-1][0]}'\n")
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    subprocess.check_call(["ffmpeg", "-y", "-loglevel", "error", "-f", "concat", "-safe", "0",
                           "-i", listing, "-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2,format=yuv420p",
                           "-r", "10", "-c:v", "libx264", "-crf", "23", args.out])
    shutil.rmtree(work, ignore_errors=True)
    total = sum(r for _, r in frames) / FPS
    print(f"{args.out}: {total:.0f} s of video, guest boot took {boot_seconds:.0f} s")


if __name__ == "__main__":
    main()
