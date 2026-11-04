#!/usr/bin/env python3
"""TRACK's logo (r549, Tobias 11-04 chose "1 Boelgeform"): a dark tile, the Track page's gold
waveform and a white playhead. Flat - no gradients, glow or shadows.

    python3 trackbridge/branding/make_logo.py      # writes track.png, track-512.png, track.ico here
"""
from pathlib import Path
from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
S = 1024
GOLD, DIM, DARK, WHITE = (227, 180, 79), (110, 96, 64), (20, 20, 24), (255, 255, 255)


def draw():
    im = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((32, 32, S - 32, S - 32), radius=200, fill=DARK)
    heights = [0.30, 0.55, 0.42, 0.80, 0.62, 0.95, 0.70, 0.48, 0.85, 0.38, 0.60, 0.30]
    x0, w = 150, (S - 300) / len(heights)
    for i, h in enumerate(heights):
        bh, cx = h * 560, x0 + i * w
        d.rounded_rectangle((cx + 8, S / 2 - bh / 2, cx + w - 8, S / 2 + bh / 2), radius=18,
                            fill=GOLD if i < 7 else DIM)
    px = x0 + 7 * w - 2
    d.rectangle((px - 10, 170, px + 10, S - 170), fill=WHITE)
    d.polygon([(px - 46, S - 170), (px + 46, S - 170), (px, S - 226)], fill=WHITE)
    return im


if __name__ == '__main__':
    im = draw()
    im.resize((256, 256), Image.LANCZOS).save(HERE / 'track.png')
    im.resize((512, 512), Image.LANCZOS).save(HERE / 'track-512.png')
    im.save(HERE / 'track.ico', sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    print('track.png, track-512.png, track.ico')
