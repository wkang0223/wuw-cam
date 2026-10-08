# WUW BWO shrine art

Original artwork generated with the built-in image-generation tool. The visual
reference is early PlayStation adventure RPGs and Korean fantasy MMOs, with
original camera pilgrims and shrine guardians rather than franchise characters.

## Assets

- Source: `assets/wuw-rpg-source.png`.
- Firmware: `assets/wuw-rpg-atlas.inc`, 16 RGB565 tiles at 32x32, 32768 bytes in flash.
- Browser: `site/game-preview/art/tile-0.png` through `tile-15.png`, 64x64.
- Offline browser atlas: `site/game-preview/art/inline.js`.
- Tile order: pilgrim idle/step; guardian idle/step; memory crystal; shutter flower;
  portal; portrait; stone wall; jade panel; red ceramic; machinery; stone floor;
  coffered ceiling; wood floor; engraved stone.

Transparent sprite pixels are encoded as zero in the firmware. The atlas is
shared between BWO and the ritual HUDs and is never decoded during TFT play.
The existing four user-photo slots remain independent of the default materials.

## Regeneration

`tools/build_game_art.cjs` uses Sharp to slice the source, reduce it to the two
target sizes, retain sprite transparency, and encode the firmware array.
Run with `node tools/build_game_art.cjs assets/wuw-rpg-source.png` where Sharp is
installed, or set `WUW_SHARP` to the installed Sharp package path.

The preview uses locally vendored Three.js 0.180.0, with its license under
`site/game-preview/vendor/LICENSE`. Rebuild the classic script with:

```sh
npm exec --package esbuild@0.25.10 -- esbuild site/game-preview/rpg-renderer.js --bundle --format=iife --minify --outfile=site/game-preview/rpg.bundle.js
```

Browser modes: diorama, first person, and TFT-style software rendering.
Only the software renderer and 32px atlas run on the S3. Browser 3D uses the
computer/phone GPU. Hardware FPS has not been measured for this revision.

## Generation prompt

Use case: stylized-concept. Asset type: production game sprite and texture atlas for WUW BWO camera ritual RPG. Create one square 1024x1024 image with EXACTLY 4 columns and 4 rows of equal square cells (256x256 each), no gaps, no text, no labels, no outlines around cells. Original early PlayStation / PS2 era Japanese adventure RPG and Korean isometric fantasy MMO aesthetics: charming yet uncanny cyber shrine, hand-painted textured low-poly feel, readable chunky silhouettes, restrained jade, ivory, burgundy and silver colors. TOP TWO ROWS: eight game sprites, each fully contained centered with generous 15% pure black background margin, full body or full object front-three-quarter view, crisp opaque hard edges with black background #000000 (used as transparency), no floor or cast shadow. Row1 cells left to right: camera-headed shrine pilgrim ivory coat jade lens idle; SAME pilgrim stepping pose; silver and burgundy mechanical masked guardian idle; SAME guardian stepping pose. Row2 left to right: jade faceted memory crystal suspended in silver artifact frame; ivory and red shutter-flower relic; small ornate stone shrine arch portal with jade lit interior; small camera pilgrim portrait bust on black. BOTTOM TWO ROWS: seamless tiling hand-painted environment square textures that fill entire cell edge to edge, flat orthographic surface no perspective: row3 weathered ivory stone block wall; muted jade oxidized metal panel; burgundy embossed shrine ceramic; silver vented machinery panel. Row4 grey-green stone floor pavers; coffered ivory ceiling tiles; dark red-brown wooden floor boards; slate stone engraved ritual floor tile. Every cell must remain within its exact quarter-grid region. No existing franchise characters or logos. Game-ready atlas, sharp stylized material detail, no glow leaking beyond silhouette, no captions.

## Verification

- `wuwcam` and `wuwcam-ov3660` compile successfully, 150640 bytes static RAM.
- Browser screenshots checked at desktop and 390x844: diorama, first person,
  TFT view, level change, cube placement, imported image on sides and top.
- Collective Shutter and shared ritual HUD artwork integrated.
- Browser console reported no warnings/errors during the exercised flow.
- Direct-file test was blocked by the browser tool URL policy. Classic scripts
  and inline atlas remove module imports and image-origin dependencies from that path.
- Physical display, SD loading, and runtime FPS require a device test after flashing.

## W8I, the player pilgrim

The camera-box pilgrim was replaced by W8I (reference sheet: `assets/w8i-reference.webp`): a crimson shaggy mullet,
face chains and tattoos, dressed in a pale snow-gothic outfit (ash-grey military coat with a fur collar and pewter
buttons over a bone-white blouse, a pale knight's cloak with a pewter cross, strapped black cargo trousers, white
cutwork lace-up boots, a pink WUW CAM in hand). Only three shrine-atlas tiles changed: 0 (idle), 1 (walk) and 7
(portrait, the HUD face on every BWO screen). The other 45 tiles, the thorn and tendril atlases and the world
art are untouched.

```sh
python3 tools/make_w8i.py /tmp/w8i                                  # draws idle.png, step.png, portrait.png (about a minute)
python3 tools/apply_w8i.py /tmp/w8i assets/wuw-rpg-source.png       # pastes them into tiles 0, 1 and 7, keeping the alpha channel
WUW_SHARP=/path/to/sharp node tools/build_game_art.cjs assets/wuw-rpg-source.png assets/wuw-thorn-source.png assets/wuw-tendril-source.png
```

The art is flat cel-shaded shapes drawn at 4x, so it survives the 64 px and 32 px reductions. Two rules matter:
the build turns any sprite pixel whose channels are all under 35 transparent, so nothing in the figure may be
near-black (the outline is dark plum, the trousers dark grey); and the source atlas has an alpha channel that the
other sprites depend on, so cells are pasted as RGBA, never flattened. The firmware compiles unchanged
(`pio run -e wuwcam` succeeds); it has not been flashed or seen on the TFT. The 32 px portrait is drawn 16 px wide
in the HUD, which is why the face is a bold red-hair-and-skin shape rather than fine detail.
