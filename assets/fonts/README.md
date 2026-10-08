# WUW panel font

The on-device panel font is a 1-bit raster of **Silkscreen** by Jason Kottke,
licensed under the SIL Open Font License 1.1 (`OFL.txt` in this folder). The
raster is embedded in `ui_font.cpp`, which the OFL permits as long as the
license and copyright notice travel with it.

Silkscreen is a pixel font on an 8 px grid. It draws lowercase as small caps and
has no descenders.

Regenerate `ui_font.cpp` with Pillow installed:

```sh
python3 tools/build_ui_font.py assets/fonts/Silkscreen-Regular.ttf ui_font.cpp
```

Options: `--size` (default 10) and `--threshold` (default 80).

Earlier builds used two personal-use-only fonts. They have been removed from the
tree and are not part of this repository.
