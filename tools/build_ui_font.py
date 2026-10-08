#!/usr/bin/env python3
"""Build the WUW panel's GFX font from a single TTF (Silkscreen, SIL OFL 1.1).

Output is a 1-bit LovyanGFX/Adafruit GFX font kept in flash. Silkscreen is a
pixel font on an 8 px grid; the default size of 10 was picked to keep glyph
advances close to the layout the panel was drawn for.
"""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


FIRST = 0x20
LAST = 0x7E


def packed_bitmap(font: ImageFont.FreeTypeFont, char: str, threshold: int):
    left, top, right, bottom = font.getbbox(char, anchor="ls")
    width = max(0, right - left)
    height = max(0, bottom - top)
    advance = max(1, round(font.getlength(char)))
    if not width or not height:
        return b"", width, height, advance, left, top

    image = Image.new("L", (width, height), 0)
    ImageDraw.Draw(image).text((-left, -top), char, font=font, fill=255)
    bits: list[int] = []
    for y in range(height):
        for x in range(width):
            bits.append(1 if image.getpixel((x, y)) >= threshold else 0)

    out = bytearray()
    for start in range(0, len(bits), 8):
        value = 0
        for bit in bits[start : start + 8]:
            value = (value << 1) | bit
        value <<= max(0, 8 - len(bits[start : start + 8]))
        out.append(value)
    return bytes(out), width, height, advance, left, top


def emit(font_path: Path, output: Path, size: int, threshold: int) -> None:
    font = ImageFont.truetype(str(font_path), size)

    bitmap = bytearray()
    glyphs = []
    for code in range(FIRST, LAST + 1):
        char = chr(code)
        data, width, height, advance, xoff, yoff = packed_bitmap(
            font, char, threshold
        )
        glyphs.append((len(bitmap), width, height, advance, xoff, yoff, char))
        bitmap.extend(data)

    lines = [
        '#include "ui_font.h"',
        '#include <Arduino.h>',
        '',
        'static const uint8_t WUW_FONT_BITMAPS[] PROGMEM = {',
    ]
    for start in range(0, len(bitmap), 16):
        chunk = bitmap[start : start + 16]
        lines.append("  " + ", ".join(f"0x{value:02X}" for value in chunk) + ",")
    lines.extend([
        '};',
        '',
        'static const lgfx::GFXglyph WUW_FONT_GLYPHS[] PROGMEM = {',
    ])
    for offset, width, height, advance, xoff, yoff, char in glyphs:
        label = "space" if char == " " else char.replace("\\", "backslash")
        lines.append(
            f"  {{ {offset:4d}, {width:2d}, {height:2d}, {advance:2d}, "
            f"{xoff:3d}, {yoff:3d} }}, // 0x{ord(char):02X} {label}"
        )
    lines.extend([
        '};',
        '',
        'const lgfx::GFXfont WUW_UI_FONT PROGMEM = {',
        '  (uint8_t*)WUW_FONT_BITMAPS,',
        '  (lgfx::GFXglyph*)WUW_FONT_GLYPHS,',
        f'  0x{FIRST:02X}, 0x{LAST:02X}, {size + 3}',
        '};',
        '',
        f'// {len(bitmap)} bitmap bytes; Silkscreen {size}px (SIL OFL 1.1).',
    ])
    output.write_text("\n".join(lines) + "\n", encoding="ascii")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("font", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--size", type=int, default=10)
    parser.add_argument("--threshold", type=int, default=80)
    args = parser.parse_args()
    emit(args.font, args.output, args.size, args.threshold)


if __name__ == "__main__":
    main()
