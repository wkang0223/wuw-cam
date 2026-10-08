#!/usr/bin/env python3
"""Paste W8I's idle, step and portrait cells into the shrine atlas (tiles 0, 1 and 7).

    python3 tools/apply_w8i.py cells_dir assets/wuw-rpg-source.png

The cell boundaries replicate tools/build_game_art.cjs exactly (cell = width / 4, JS Math.round).
"""
import math
import sys
from PIL import Image

def jsround(x):
    return int(math.floor(x + 0.5))

cells_dir, atlas_path = sys.argv[1], sys.argv[2]
atlas = Image.open(atlas_path).convert("RGBA")   # keep the alpha channel: the other sprites rely on it
cell = atlas.width / 4
assert atlas.width == atlas.height
for name, tile in (("idle", 0), ("step", 1), ("portrait", 7)):
    col, row = tile % 4, tile // 4
    left, right = jsround(col * cell), jsround((col + 1) * cell)
    top, bottom = jsround(row * cell), jsround((row + 1) * cell)
    art = Image.open(f"{cells_dir}/{name}.png").convert("RGBA")   # opaque on a black background, which the build keys out
    w, h = right - left, bottom - top
    atlas.paste(art.crop((0, 0, w, h)), (left, top))   # a straight copy, alpha included
    print(f"{name:9s} -> tile {tile} at ({left},{top}) {w}x{h}")
atlas.save(atlas_path)
