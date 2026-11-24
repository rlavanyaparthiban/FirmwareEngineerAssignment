#!/usr/bin/env python3
"""Generate the test-chart frames played by libcamera's virtual camera.

The chart is synthetic (no third-party imagery) and built so that the software
ISP can be judged by eye and by number:

  * colour bars and complementary colour patches   -> CCM, white balance
  * grey ramp and grey steps                       -> black level, gamma
  * zone plate and slanted edges                   -> demosaic artefacts
  * a disc pair that moves from frame to frame     -> shows live video

Its linear-light average is neutral grey at 18 %, which is exactly the scene
that gray-world white balance and a mean-luminance auto exposure assume.

Usage: gen-test-frames.py OUTPUT_DIR [--frames N] [--size WxH]
Requires: numpy, Pillow.
"""
import argparse
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFont

TARGET_MEAN = 0.18  # linear-light scene average


def srgb_encode(lin):
    lin = np.clip(lin, 0.0, 1.0)
    return np.where(lin <= 0.0031308, lin * 12.92, 1.055 * np.power(lin, 1 / 2.4) - 0.055)


def srgb_decode(v):
    return np.where(v <= 0.04045, v / 12.92, np.power((v + 0.055) / 1.055, 2.4))


def build_static(w, h, rng):
    """Returns (linear RGB image, mask of pixels that belong to the background)."""
    img = np.zeros((h, w, 3), np.float64)
    bg = np.ones((h, w), bool)

    def rect(x0, y0, x1, y1, colour):
        img[y0:y1, x0:x1] = colour
        bg[y0:y1, x0:x1] = False

    m = w // 32  # margin

    # 75 % colour bars: white yellow cyan green magenta red blue black.
    bars = [(1, 1, 1), (1, 1, 0), (0, 1, 1), (0, 1, 0), (1, 0, 1), (1, 0, 0), (0, 0, 1), (0, 0, 0)]
    bw = (w - 2 * m) // len(bars)
    for i, c in enumerate(bars):
        rect(m + i * bw, m, m + (i + 1) * bw, m + h // 5, srgb_decode(np.array(c) * 0.75))

    # Grey ramp (linear in code value) and 11 grey steps.
    y0 = m + h // 5 + m // 2
    ramp = srgb_decode(np.linspace(0.0, 1.0, w - 2 * m))
    img[y0:y0 + h // 14, m:w - m] = ramp[None, :, None]
    bg[y0:y0 + h // 14, m:w - m] = False
    y1 = y0 + h // 14
    sw = (w - 2 * m) // 11
    for i in range(11):
        rect(m + i * sw, y1, m + (i + 1) * sw, y1 + h // 14, srgb_decode(np.full(3, i / 10.0)))

    # Colour patches in complementary pairs: each pair averages to grey.
    y2 = y1 + h // 14 + m // 2
    cols, ph = 12, h // 9
    pw = (w // 2 - m) // (cols // 2)
    for i in range(cols // 2):
        c = rng.uniform(0.02, 0.60, 3)
        rect(m + i * pw + 4, y2, m + (i + 1) * pw - 4, y2 + ph, c)
        rect(m + i * pw + 4, y2 + ph + 8, m + (i + 1) * pw - 4, y2 + 2 * ph + 8, 0.62 - c)

    # Zone plate: spatial frequencies from zero to half of Nyquist, in grey.
    zs = min(h // 3, w // 4)
    zx, zy = w - m - zs, y2
    yy, xx = np.mgrid[0:zs, 0:zs]
    r2 = (xx - zs / 2) ** 2 + (yy - zs / 2) ** 2
    zone = 0.18 + 0.16 * np.cos(0.5 * np.pi * r2 / zs)
    img[zy:zy + zs, zx:zx + zs] = zone[..., None]
    bg[zy:zy + zs, zx:zx + zs] = False

    # Slanted black/white edges.
    ex, ey, es = w // 2 + m, y2, zs // 2
    yy, xx = np.mgrid[0:es, 0:es]
    edge = np.where(xx + 0.2 * yy > es * 0.6, 0.80, 0.02)
    img[ey:ey + es, ex:ex + es] = edge[..., None]
    bg[ey:ey + es, ex:ex + es] = False

    return img, bg


def add_moving(img, bg, w, h, t):
    """A white and a black disc, travelling in opposite directions."""
    r = h // 16
    cy = h - h // 8
    span = w - 6 * r
    yy, xx = np.mgrid[0:h, 0:w]
    for value, phase in ((0.80, t), (0.02, 1.0 - t)):
        cx = 3 * r + phase * span
        disc = (xx - cx) ** 2 + (yy - cy) ** 2 <= r * r
        img[disc] = value
        bg[disc] = False


def render(w, h, index, count, seed=7):
    rng = np.random.default_rng(seed)
    img, bg = build_static(w, h, rng)
    add_moving(img, bg, w, h, index / count)

    # Choose the background so that every channel averages TARGET_MEAN.
    fg_sum = img[~bg].sum(axis=0)
    background = (TARGET_MEAN * w * h - fg_sum) / bg.sum()
    if background.min() < 0.0 or background.max() > 1.0:
        raise SystemExit("chart layout cannot be balanced to the target mean")
    img[bg] = background

    out = Image.fromarray(np.round(srgb_encode(img) * 255).astype(np.uint8), "RGB")
    draw = ImageDraw.Draw(out)
    label = f"ISP CHANNEL TEST CHART  frame {index + 1}/{count}"
    draw.text((w // 32, h - h // 24), label, fill=(20, 20, 20), font=ImageFont.load_default())
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("output")
    parser.add_argument("--frames", type=int, default=8)
    parser.add_argument("--size", default="1920x1080")
    args = parser.parse_args()

    w, h = (int(v) for v in args.size.split("x"))
    os.makedirs(args.output, exist_ok=True)
    for i in range(args.frames):
        # libcamera's virtual pipeline handler expects 0.jpg, 1.jpg, ...
        # Baseline 4:2:0 JPEG is what libyuv's MJPG decoder handles.
        path = os.path.join(args.output, f"{i}.jpg")
        render(w, h, i, args.frames).save(path, "JPEG", quality=92, subsampling="4:2:0",
                                          progressive=False, optimize=False)
        lin = srgb_decode(np.asarray(Image.open(path), np.float64) / 255.0)
        print(f"{path}: linear mean R {lin[..., 0].mean():.4f} G {lin[..., 1].mean():.4f} "
              f"B {lin[..., 2].mean():.4f}")


if __name__ == "__main__":
    main()
