# WUW CAM skin borders

These 640x360 JPEGs are the flash-ready viewfinder borders used by the web UI:

- `wuw-skin-nacre.jpg`
- `wuw-skin-gunmetal.jpg`
- `wuw-skin-obsidian.jpg`
- `wuw-skin-ember.jpg`
- `wuw-skin-seance.jpg`
- `wuw-skin-hangar.jpg`
- `wuw-skin-chassis.jpg`
- `wuw-skin-sigil.jpg`
- `wuw-skin-reliquary.jpg`
- `wuw-skin-fairy.jpg`

The images are original generated raster assets. The two supplied cybercore
references were used only for tactile late-1990s/early-2000s interface mood.
The prompts required a 16:9 frame, at least 72 percent clear central viewfinder,
detail confined to the perimeter, no characters, no weapons, no readable text,
no logos, and no watermark. Each variant then used the palette and material
language named by its WUW CAM skin.

The matching `.inc` files are mechanical byte encodings consumed by
`ui_art.cpp`. Regenerate an include after changing a JPEG with:

```sh
od -An -v -tx1 image.jpg | awk '{for(i=1;i<=NF;i++) printf "0x%s,",$i; print ""}'
```

The full-resolution Nacre source is retained as
`wuw-cybercore-source.png`. Other full-resolution generation outputs remain in
the Codex generated-images store; the project deliverables are the optimized
JPEGs above.
