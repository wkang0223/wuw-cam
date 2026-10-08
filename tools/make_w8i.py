#!/usr/bin/env python3
"""Draw W8I, the BWO player character, for the shrine atlas: walk-idle, walk-step and portrait.

W8I has a shaggy crimson mullet, face chains and tattoos. The outfit borrows the pale snow-gothic fashion look:
an ash-grey military coat with a fur collar and pewter buttons over a bone-white blouse, strapped black cargo
trousers, and white cutwork lace-up boots. A small pink WUW CAM hangs from one hand.

Everything is drawn as flat cel-shaded shapes at 4x and reduced, so it survives the pipeline's 64 px and 32 px
reductions. No pixel may be near-black (all channels under 35) anywhere inside the figure: tools/build_game_art.cjs
turns those transparent. The background is pure black for the same reason.

    python3 tools/make_w8i.py out_dir           # writes idle.png, step.png, portrait.png (314x314 each)
"""
import math
import random
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

import os
CELL, SS = 314, int(os.environ.get('SS', '4'))
N = CELL * SS
U = N / 100.0                      # pixels per unit; the cell is 100 x 100 units

INK = (52, 36, 54)                 # outline: dark plum, never near-black
HAIR = [(112, 14, 28), (186, 24, 34), (232, 64, 52)]      # shade, base, highlight
SKIN = [(150, 100, 74), (196, 140, 104), (226, 172, 132)]
COAT = [(112, 114, 124), (150, 152, 162), (186, 188, 196)]
FUR = [(138, 130, 112), (178, 170, 150), (222, 216, 198)]
BLOUSE = [(190, 186, 178), (234, 231, 223), (252, 250, 245)]
CARGO = [(52, 50, 62), (76, 74, 88), (104, 102, 118)]
BOOT = [(176, 170, 156), (226, 222, 210), (250, 247, 240)]
PEWTER = [(96, 98, 108), (160, 162, 172), (226, 228, 236)]
PINK = [(176, 70, 110), (216, 111, 156), (240, 160, 196)]


def P(pts, dx=0.0, dy=0.0):
    return [((x + dx) * U, (y + dy) * U) for x, y in pts]


class Canvas:
    def __init__(self):
        self.img = np.zeros((N, N, 3), np.float32)

    def _mask(self, polys=(), ellipses=(), lines=()):
        m = Image.new("L", (N, N), 0)
        d = ImageDraw.Draw(m)
        for pts in polys:
            d.polygon(pts, fill=255)
        for box in ellipses:
            d.ellipse(box, fill=255)
        for pts, w in lines:
            d.line(pts, fill=255, width=max(1, int(w * U)), joint="curve")
        return m

    def part(self, colors, polys=(), ellipses=(), lines=(), ow=0.9, ink=INK, shade="x", ink_on=True):
        """A shape with an outline and a light-to-dark gradient. colors = (shade, base, highlight)."""
        m = self._mask(polys, ellipses, lines)
        a = np.asarray(m, np.float32) / 255.0
        if not a.any():
            return
        if ink_on and ow > 0:
            size = int(2 * ow * U) | 1
            dil = np.asarray(m.filter(ImageFilter.MaxFilter(size)), np.float32) / 255.0
            self.img = self.img * (1 - dil[..., None]) + np.array(ink, np.float32) * dil[..., None]
        ys, xs = np.where(a > 0)
        x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
        gx = np.linspace(0, 1, x1 - x0, dtype=np.float32)[None, :]
        gy = np.linspace(0, 1, y1 - y0, dtype=np.float32)[:, None]
        t = gx if shade == "x" else gy if shade == "y" else (gx * 0.6 + gy * 0.4)
        shade_c, base_c, high_c = (np.array(c, np.float32) for c in colors)
        low = np.where(t[..., None] < 0.5, high_c * (1 - t[..., None] * 2) + base_c * (t[..., None] * 2),
                       base_c * (2 - t[..., None] * 2) + shade_c * (t[..., None] * 2 - 1))
        fill = np.zeros_like(self.img)
        fill[y0:y1, x0:x1] = low if low.shape[0] == y1 - y0 else low
        self.img = self.img * (1 - a[..., None]) + fill * a[..., None]

    def paint(self, color, polys=(), ellipses=(), lines=(), clip=None, alpha=1.0):
        m = self._mask(polys, ellipses, lines)
        a = np.asarray(m, np.float32) / 255.0 * alpha
        if clip is not None:
            a = a * (np.asarray(clip, np.float32) / 255.0)
        self.img = self.img * (1 - a[..., None]) + np.array(color, np.float32) * a[..., None]

    def mask_of(self, polys=(), ellipses=()):
        return self._mask(polys, ellipses)

    def result(self):
        out = Image.fromarray(np.clip(self.img, 0, 255).astype(np.uint8), "RGB")
        return out.resize((CELL, CELL), Image.LANCZOS)


# ───────────────────────── the head, in head units: face half-width 1, centre (0, 0)
def head(c, cx, cy, s, portrait=False, seed=7):
    rnd = random.Random(seed)

    def H(pts):
        return P([(cx + x * s, cy + y * s) for x, y in pts])

    def E(x, y, rx, ry):
        return [((cx + (x - rx) * s) * U, (cy + (y - ry) * s) * U, (cx + (x + rx) * s) * U, (cy + (y + ry) * s) * U)]

    ow = 0.55 if not portrait else 0.9
    # back hair: long mullet behind the neck, jagged ends
    back = [(-1.55, -0.9), (-1.75, 0.4), (-1.95, 1.6), (-1.7, 2.7), (-1.35, 2.2), (-1.1, 3.1), (-0.7, 2.4), (-0.35, 3.3),
            (0.0, 2.5), (0.35, 3.3), (0.7, 2.4), (1.1, 3.1), (1.35, 2.2), (1.7, 2.7), (1.95, 1.6), (1.75, 0.4),
            (1.55, -0.9), (1.2, -1.6), (0.0, -1.95), (-1.2, -1.6)]
    c.part(HAIR, polys=[H(back)], ow=ow, shade="y")
    hair_clip = c.mask_of(polys=[H(back)])
    for _ in range(26 if not portrait else 60):
        x = rnd.uniform(-1.8, 1.8)
        y0 = rnd.uniform(-0.4, 1.4)
        y1 = y0 + rnd.uniform(0.9, 1.7)
        c.paint(rnd.choice([HAIR[0], HAIR[2], HAIR[1]]), lines=[(H([(x, y0), (x + rnd.uniform(-0.12, 0.12), y1)]), 0.05 * (3 if portrait else 1))], clip=hair_clip, alpha=0.8)
    # neck and its tattoo
    c.part(SKIN, polys=[H([(-0.42, 0.8), (0.42, 0.8), (0.5, 2.2), (-0.5, 2.2)])], ow=ow, shade="x")
    c.paint((70, 44, 56), polys=[H([(-0.05, 1.3), (0.05, 1.3), (0.05, 1.75), (-0.05, 1.75)]),
                                 H([(-0.2, 1.45), (0.2, 1.45), (0.2, 1.52), (-0.2, 1.52)]),
                                 H([(-0.14, 1.62), (0.14, 1.62), (0.14, 1.68), (-0.14, 1.68)])])
    if not portrait:
        return back
    return back


def face(c, cx, cy, s, portrait=False, seed=7):
    rnd = random.Random(seed + 1)

    def H(pts):
        return P([(cx + x * s, cy + y * s) for x, y in pts])

    def E(x, y, rx, ry):
        return [((cx + (x - rx) * s) * U, (cy + (y - ry) * s) * U, (cx + (x + rx) * s) * U, (cy + (y + ry) * s) * U)]

    ow = 0.5 if not portrait else 0.85
    # ears with a ring each
    for sx in (-1, 1):
        c.part(SKIN, ellipses=E(sx * 1.0, 0.15, 0.17, 0.3), ow=ow * 0.8, shade="x")
    # face: a soft oval with a tapering jaw
    pts = []
    for i in range(0, 72):
        t = i / 72 * 2 * math.pi
        x, y = math.cos(t), math.sin(t) * 1.2
        if math.sin(t) > 0:
            x *= 1 - 0.30 * math.sin(t) ** 1.6
        pts.append((x, y))
    c.part(SKIN, polys=[H(pts)], ow=ow, shade="d")
    # cheek and brow shade
    c.paint(SKIN[0], polys=[H([(0.55, 0.3), (0.95, 0.15), (0.8, 0.8), (0.5, 1.0)])], clip=c.mask_of(polys=[H(pts)]), alpha=0.55)
    # eyes, brows, nose, mouth
    for sx in (-1, 1):
        c.paint((58, 36, 40), polys=[H([(sx * 0.18, -0.2 + 0.0), (sx * 0.62, -0.28), (sx * 0.66, -0.2), (sx * 0.2, -0.1)])])
        c.paint((46, 30, 44), ellipses=E(sx * 0.42, 0.04, 0.2, 0.15))
        c.paint((250, 246, 240), ellipses=E(sx * 0.42 - 0.07, -0.02, 0.05, 0.05))
    c.paint(SKIN[0], polys=[H([(0.0, 0.1), (0.1, 0.5), (-0.1, 0.5)])], alpha=0.8)
    c.paint((150, 54, 64), polys=[H([(-0.22, 0.78), (0.0, 0.74), (0.22, 0.78), (0.0, 0.86)])])
    # face chains: one across the mouth, one along the jaw, with beads
    for chain in ([(-1.0, 0.3), (-0.55, 0.66), (0.0, 0.74), (0.55, 0.66), (1.0, 0.3)],
                  [(-1.02, 0.42), (-0.85, 0.85), (-0.45, 1.08), (0.0, 1.15), (0.45, 1.08), (0.85, 0.85), (1.02, 0.42)]):
        c.paint((176, 182, 198), lines=[(H(chain), 0.06 if not portrait else 0.05)])
        for (x0, y0), (x1, y1) in zip(chain, chain[1:]):
            for k in range(2):
                bx, by = x0 + (x1 - x0) * (k + 0.5) / 2, y0 + (y1 - y0) * (k + 0.5) / 2
                c.paint((238, 242, 252), ellipses=E(bx, by, 0.042, 0.042))
    for sx in (-1, 1):
        c.paint((226, 230, 240), ellipses=E(sx * 1.04, 0.36, 0.08, 0.08))
    # forehead glyph
    c.paint((150, 70, 70), polys=[H([(-0.03, -0.62), (0.03, -0.62), (0.03, -0.42), (-0.03, -0.42)]),
                                  H([(-0.12, -0.55), (0.12, -0.55), (0.12, -0.5), (-0.12, -0.5)])], alpha=0.7)
    # front hair: a split fringe in jagged strands, then the crown and the side locks
    fringe = [(-1.12, -0.95), (-1.0, -0.2), (-0.88, -0.78), (-0.72, 0.0), (-0.56, -0.8), (-0.38, -0.1), (-0.2, -0.85),
              (0.0, -0.3), (0.2, -0.85), (0.4, -0.12), (0.58, -0.8), (0.74, 0.02), (0.9, -0.78), (1.02, -0.18), (1.12, -0.95),
              (0.9, -1.25), (0.0, -1.35), (-0.9, -1.25)]
    crown = [(-1.4, -0.5), (-1.75, -1.05), (-1.4, -1.45), (-1.62, -2.0), (-0.95, -1.72), (-0.72, -2.3), (-0.28, -1.85), (0.08, -2.42),
             (0.5, -1.85), (0.95, -2.22), (1.12, -1.62), (1.66, -1.82), (1.52, -1.12), (1.78, -0.78), (1.4, -0.5), (1.0, -1.0), (0.0, -1.25), (-1.0, -1.0)]
    locks = [[(-1.0, -0.7), (-1.5, -0.2), (-1.65, 1.2), (-1.4, 2.2), (-1.2, 1.6), (-1.05, 2.4), (-0.95, 1.0), (-0.82, 0.2)],
             [(1.0, -0.7), (1.5, -0.2), (1.65, 1.2), (1.4, 2.2), (1.2, 1.6), (1.05, 2.4), (0.95, 1.0), (0.82, 0.2)]]
    c.part(HAIR, polys=[H(p) for p in locks], ow=ow, shade="y")
    c.part(HAIR, polys=[H(crown)], ow=ow, shade="y")
    c.part(HAIR, polys=[H(fringe)], ow=ow, shade="y")
    clip = c.mask_of(polys=[H(crown), H(fringe)] + [H(p) for p in locks])
    for _ in range(22 if not portrait else 46):
        x = rnd.uniform(-1.4, 1.4)
        y0 = rnd.uniform(-1.9, -0.6)
        y1 = y0 + rnd.uniform(0.6, 1.5)
        c.paint(rnd.choice([HAIR[2], HAIR[0], HAIR[2]]), lines=[(H([(x, y0), (x * 1.04 + rnd.uniform(-0.1, 0.1), y1)]), 0.045 * (2.6 if portrait else 1))], clip=clip, alpha=0.75)


HEAD_S = 9.3


def pose_figure(step=False):
    c = Canvas()
    bob = -1.2 if step else 0.0
    sw = 4.0 if step else 0.0                 # leg swing

    def D(pts, dy=bob):
        return P(pts, 0, dy)

    # back hair, behind the shoulders
    head(c, 50, 23.5, HEAD_S, seed=7)
    # legs: strapped black cargo trousers and white cutwork boots
    for side in (-1, 1):
        lift = (-4.6 if side < 0 else 1.8) if step else 0.0
        out = (side * 1.5 + (-5.0 if side < 0 else 3.5)) if step else side * 0.0
        x0 = 50 + side * 6.0
        # trouser leg
        c.part(CARGO, polys=[P([(x0 - 6.0, 62), (x0 + 6.0, 62), (x0 + 5.4 + out * 0.3, 79 + lift), (x0 - 5.4 + out * 0.3, 79 + lift)], 0, bob)], ow=0.7, shade="x")
        c.paint(CARGO[2], polys=[P([(x0 - 3.2 + side * 0.4, 66), (x0 + 0.6, 66), (x0 + 0.6, 71), (x0 - 3.2 + side * 0.4, 71)], 0, bob)], alpha=0.55)
        c.part(PEWTER, polys=[P([(x0 + side * 4.2 - 0.5, 63), (x0 + side * 4.2 + 0.5, 63), (x0 + side * 4.6 + 0.5, 77 + lift), (x0 + side * 4.6 - 0.5, 77 + lift)], 0, bob)], ow=0.35, shade="y")
        # boot: tall, bone white, three cutwork slits and a chunky sole
        bx = x0 + out * 0.5
        top = 77 + lift
        c.part(BOOT, polys=[P([(bx - 5.4, top), (bx + 5.4, top), (bx + 5.8, 92 + lift), (bx + 8.2 * (1 if side > 0 else 0.6), 94.5 + lift),
                              (bx + 7.6 * (1 if side > 0 else 0.5), 97 + lift), (bx - 7.4 * (1 if side < 0 else 0.5), 97 + lift),
                              (bx - 6.0, 94 + lift), (bx - 5.8, 90 + lift)], 0, bob)], ow=0.8, shade="x")
        for k in range(3):
            yy = top + 3.0 + k * 3.8
            c.paint((82, 60, 80), polys=[P([(bx - 2.6, yy), (bx + 2.6, yy + 0.6), (bx + 2.4, yy + 2.2), (bx - 2.8, yy + 1.6)], 0, bob)])
        c.paint((120, 116, 130), lines=[(P([(bx - 3.0, top + 1.2), (bx + 3.0, top + 12.5)], 0, bob), 0.28), (P([(bx + 3.0, top + 1.2), (bx - 3.0, top + 12.5)], 0, bob), 0.28)], alpha=0.8)
        c.part(PEWTER, polys=[P([(bx - 7.0 * (1 if side < 0 else 0.6), 96.4 + lift), (bx + 7.6 * (1 if side > 0 else 0.6), 96.4 + lift),
                                (bx + 7.8 * (1 if side > 0 else 0.6), 99 + lift), (bx - 7.4 * (1 if side < 0 else 0.6), 99 + lift)], 0, bob)], ow=0.6, shade="y")
    # coat tails (behind the sleeves)
    for side in (-1, 1):
        sway = (side * 2.2 if step else 0.0)
        c.part(COAT, polys=[P([(50 + side * 1.0, 52), (50 + side * 17.0, 52), (50 + side * 21.5 + sway, 67), (50 + side * 17.5 + sway, 65), (50 + side * 14.0 + sway, 69), (50 + side * 8.0, 65.5), (50 + side * 1.5, 67)], 0, bob)], ow=0.8, shade="x")
    # torso: coat with a bone-white blouse down the front
    c.part(COAT, polys=[D([(34.5, 36), (65.5, 36), (68.5, 47), (69.5, 58), (50.0, 62), (30.5, 58), (31.5, 47)])], ow=0.85, shade="x")
    c.part(BLOUSE, polys=[D([(44.8, 37), (55.2, 37), (54.2, 62), (45.8, 62)])], ow=0.5, shade="x")
    for k in range(5):
        c.part(PEWTER, ellipses=[((50 - 0.9) * U, (41.4 + k * 4.6 + bob - 0.9) * U, (50 + 0.9) * U, (41.4 + k * 4.6 + bob + 0.9) * U)], ow=0.18, shade="d")
    # jabot ruffle at the throat
    c.part(BLOUSE, polys=[D([(44.0, 35.5), (48.0, 38.0), (50.0, 36.5), (52.0, 38.0), (56.0, 35.5), (55.0, 41.0), (50.0, 43.0), (45.0, 41.0)])], ow=0.5, shade="y")
    # coat lapel edges and belt
    c.paint((84, 86, 98), polys=[D([(44.2, 37), (45.0, 37), (46.2, 62), (45.2, 62)]), D([(55.0, 37), (55.8, 37), (54.8, 62), (53.8, 62)])])
    c.part(CARGO, polys=[D([(31.0, 53.2), (69.0, 53.2), (69.2, 56.0), (30.8, 56.0)])], ow=0.5, shade="y")
    c.part(PEWTER, polys=[D([(48.2, 52.8), (51.8, 52.8), (51.8, 56.4), (48.2, 56.4)])], ow=0.35, shade="d")
    # hanging black straps with pewter buckles
    for sx_ in (39.0, 61.0):
        c.part(CARGO, polys=[D([(sx_ - 0.8, 56), (sx_ + 0.8, 56), (sx_ + 1.0, 71), (sx_ - 1.0, 71)])], ow=0.35, shade="y")
        c.part(PEWTER, polys=[D([(sx_ - 1.5, 67.5), (sx_ + 1.5, 67.5), (sx_ + 1.5, 70.5), (sx_ - 1.5, 70.5)])], ow=0.3, shade="d")
    # a small damask cross on the coat
    c.paint(PEWTER[2], polys=[D([(39.4, 43.0), (40.6, 43.0), (40.6, 50.0), (39.4, 50.0)]), D([(37.2, 45.0), (42.8, 45.0), (42.8, 46.2), (37.2, 46.2)])])
    # fur collar around the neck
    collar = [(35.0, 37.5), (37.0, 31.0), (41.0, 33.5), (43.0, 29.5), (47.5, 33.0), (50.0, 30.5), (52.5, 33.0), (57.0, 29.5), (59.0, 33.5), (63.0, 31.0), (65.0, 37.5), (62.0, 43.0), (56.0, 41.0), (50.0, 44.0), (44.0, 41.0), (38.0, 43.0)]
    c.part(FUR, polys=[D(collar)], ow=0.7, shade="y")
    fur_clip = c.mask_of(polys=[D(collar)])
    rnd = random.Random(3)
    for _ in range(46):
        x, y = rnd.uniform(36, 64), rnd.uniform(30, 43)
        c.paint(FUR[2], lines=[(D([(x, y), (x + rnd.uniform(-1.2, 1.2), y + rnd.uniform(1.2, 2.4))]), 0.2)], clip=fur_clip, alpha=0.85)
    # sleeves and hands
    arm = (6.0 if step else 0.0)
    for side in (-1, 1):
        a = arm * (1 if side < 0 else -1)
        sx = 50 + side * 18.5
        c.part(COAT, polys=[D([(sx - side * 4.4, 37), (sx + side * 3.6, 38), (sx + side * 4.6 + a * side * 0.4, 57 + a * 0.3), (sx - side * 3.0 + a * side * 0.4, 59 + a * 0.3)])], ow=0.8, shade="x")
        c.part(FUR, polys=[D([(sx - side * 3.4 + a * side * 0.4, 56.6 + a * 0.3), (sx + side * 5.0 + a * side * 0.4, 55.2 + a * 0.3), (sx + side * 5.4 + a * side * 0.4, 60.0 + a * 0.3), (sx - side * 3.6 + a * side * 0.4, 61.0 + a * 0.3)])], ow=0.5, shade="y")
        hx, hy = sx + side * 0.7 + a * side * 0.4, 63.6 + a * 0.3
        c.part(SKIN, ellipses=[((hx - 2.2) * U, (hy - 2.4 + bob) * U, (hx + 2.2) * U, (hy + 2.4 + bob) * U)], ow=0.6, shade="d")
        if side < 0:
            # the pink WUW CAM held at the hip
            cx0, cy0 = hx - 8.5, hy - 1.8
            c.part(PINK, polys=[D([(cx0, cy0), (cx0 + 10.5, cy0), (cx0 + 10.5, cy0 + 6.5), (cx0, cy0 + 6.5)])], ow=0.6, shade="y")
            c.part(CARGO, polys=[D([(cx0, cy0 + 4.0), (cx0 + 10.5, cy0 + 4.0), (cx0 + 10.5, cy0 + 6.5), (cx0, cy0 + 6.5)])], ow=0.4, shade="y", ink_on=False)
            c.part(PEWTER, ellipses=[((cx0 + 5.2 - 2.0) * U, (cy0 + 3.2 - 2.0 + bob) * U, (cx0 + 5.2 + 2.0) * U, (cy0 + 3.2 + 2.0 + bob) * U)], ow=0.4, shade="d")
            c.paint((40, 30, 60), ellipses=[((cx0 + 5.2 - 1.0) * U, (cy0 + 3.2 - 1.0 + bob) * U, (cx0 + 5.2 + 1.0) * U, (cy0 + 3.2 + 1.0 + bob) * U)])
            c.paint((255, 255, 255), ellipses=[((cx0 + 4.7) * U, (cy0 + 2.6 + bob) * U, (cx0 + 5.3) * U, (cy0 + 3.2 + bob) * U)])
    # a short pale knight's cloak over one shoulder, pinned with a pewter cross
    cl = [(55.0, 36.5), (66.0, 36.5), (73.5, 42.0), (75.0, 56.0), (71.0, 61.5), (67.0, 57.5), (62.5, 62.0), (58.0, 55.0), (56.5, 46.0)]
    c.part(BLOUSE, polys=[D(cl)], ow=0.85, shade="d")
    c.paint(BLOUSE[0], polys=[D([(64.0, 40.0), (65.0, 40.0), (66.5, 58.0), (65.5, 58.0)]), D([(69.0, 42.0), (70.0, 43.0), (71.0, 57.0), (70.0, 56.0)])], alpha=0.7)
    c.paint(PEWTER[2], polys=[D([(60.4, 41.0), (61.6, 41.0), (61.6, 47.0), (60.4, 47.0)]), D([(58.4, 43.0), (63.6, 43.0), (63.6, 44.2), (58.4, 44.2)])])
    # face and front hair last
    face(c, 50, 23.5, HEAD_S, seed=7)
    return c.result()


def portrait_figure():
    c = Canvas()
    # shoulders and fur collar behind the head
    c.part(COAT, polys=[P([(0, 100), (3, 84), (20, 74), (50, 70), (80, 74), (97, 84), (100, 100)])], ow=1.0, shade="x")
    c.part(BLOUSE, polys=[P([(42, 74), (58, 74), (60, 100), (40, 100)])], ow=0.6, shade="x")
    for k in range(2):
        c.part(PEWTER, ellipses=[P([(49, 86 + k * 8), (51, 88 + k * 8)])[0] + P([(49, 86 + k * 8), (51, 88 + k * 8)])[1]], ow=0.2, shade="d")
    head(c, 50, 42, 21.0, portrait=True, seed=11)
    collar = [(18, 88), (22, 74), (30, 79), (36, 71), (50, 78), (64, 71), (70, 79), (78, 74), (82, 88), (72, 100), (28, 100)]
    c.part(FUR, polys=[P(collar)], ow=0.9, shade="y")
    fur_clip = c.mask_of(polys=[P(collar)])
    rnd = random.Random(5)
    for _ in range(80):
        x, y = rnd.uniform(18, 82), rnd.uniform(72, 98)
        c.paint(FUR[2], lines=[(P([(x, y), (x + rnd.uniform(-2.5, 2.5), y + rnd.uniform(2.5, 5))]), 0.55)], clip=fur_clip, alpha=0.85)
    face(c, 50, 42, 21.0, portrait=True, seed=11)
    return c.result()


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    pose_figure(False).save(f"{out}/idle.png")
    pose_figure(True).save(f"{out}/step.png")
    portrait_figure().save(f"{out}/portrait.png")
    print("wrote idle.png, step.png, portrait.png")
