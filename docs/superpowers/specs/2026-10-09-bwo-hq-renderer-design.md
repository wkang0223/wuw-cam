# BWO HQ: browser renderer and camera system

Status: design approved 2026-10-09. Sub-projects A (renderer) + C (cameras).

## Context

BWO is the WUW CAM's game: a grid world (24x24 cells per level, several
levels) with crystals, hostile organs, player photos on walls, and peer
markers from WUW LINK multiplayer. It runs in two places:

- **Camera panel** (`bwo_game.cpp`): a DDA raycaster on a 320x240 SPI panel.
  No GPU. Out of scope here except where noted (sub-project E).
- **Browser** (`site/game-preview/`): Three.js 0.180 at a fixed 480x252,
  Lambert materials, one sun, sprites, walk and iso cameras, a glitch pass.

This spec makes the browser version photoreal and smooth with a new camera
system. The current `game-preview` stays as the fallback.

## The BWO HQ programme (for orientation)

| Id | Sub-project | Spec |
|---|---|---|
| A + C | HQ renderer and cameras | this document |
| B | W8I 3D character from `assets/w8i-reference.webp` (Blender, rigged, animated) | later |
| D | Movement AI: A* pathfinding, tap-to-move, smarter enemies | later |
| E | Panel-lite: panel sprites rendered from the B model | later |
| F | Large assets served from the camera's SD card | later |
| G | Shared world: up to 8 cameras co-edit one room | later |

## Goals

- Photoreal shrine look: the existing themes (ivory stone, jade metal, red
  ceramic, wood, coffered ceilings, engraved floors) rebuilt with PBR materials.
- Adaptive quality: one build that runs on phones and desktops.
- Smooth: phone tier 30 fps or better, desktop tier 60 fps.
- Camera system: walk, god view, bird's-eye, bullet-time orbit, panorama.
- Photo mode tied to the in-game shutter, producing path-traced stills and
  360 panoramas.

## Non-goals

- Changing game rules, save data, or the multiplayer protocol (G changes the
  protocol later).
- The W8I model itself (B). A placeholder figure is used until B lands.
- Running from the camera. Until F exists, BWO HQ runs from the website only.

## Approach

Extend Three.js WebGL2 (vendored 0.180 plus its `examples/jsm` addons).
Path tracing via `three-gpu-pathtracer` (MIT), vendored locally, used only in
photo mode. Rejected: WebGPURenderer (immature post/pathtracing, older phones
fall back anyway) and offline-baked lightmaps (the world is player-editable,
so bakes go stale exactly where people play).

## Architecture

Location: `site/bwo-hq/`, ES modules bundled with esbuild exactly like
`game-preview` (`--bundle --format=iife --minify`). All dependencies local;
no CDN, so it works offline on a camera's WiFi.

**Drop-in contract.** The HQ renderer consumes the same game state object and
exposes the same methods as `ShrineRenderer` in
`site/game-preview/rpg-renderer.js`: `rebuild(s)`, `render(s, t)`,
`syncPhotos(canvases)`, plus `setView(name)` replacing `toggleView()`.

| Module | Responsibility |
|---|---|
| `quality.js` | Tier pick at start (GPU string, devicePixelRatio, 2 s warm-up benchmark); dynamic resolution and effect toggling during play |
| `materials.js` | PBR library per theme (albedo, normal, roughness, AO); photos as framed emissive panels |
| `world.js` | Grid to bevelled instanced wall modules, trim, floors, ceilings; incremental rebuild of changed cells only |
| `lighting.js` | HDRI environment (PMREM), sun with cascaded shadow maps, a few lantern point lights, light shafts (desktop) |
| `post.js` | GTAO, bloom, SMAA/FXAA, tone mapping, ported glitch pass |
| `cameras.js` | View rigs and spring transitions |
| `photo.js` | Freeze, path-trace still or panorama, save/upload |

### Quality tiers

| Tier | Target | Features |
|---|---|---|
| Phone | >= 30 fps, 0.75x internal resolution | PBR, 1 shadow cascade, FXAA, bloom |
| Desktop | 60 fps, native resolution | 3 shadow cascades, GTAO, light shafts, SMAA |
| Photo | seconds per image | path tracing, ~256 samples |

## Cameras and photo mode

Views (one view button cycles; keys 1-3 on desktop):

- **Walk**: first person at W8I eye height, head bob, slight lean into turns.
- **God view**: high three-quarter follow; drag to orbit, pinch/scroll zoom
  from whole room to over-the-shoulder; walls between camera and W8I fade.
- **Bird's-eye**: straight down over the level, for planning; pairs with
  tap-to-move from D.

All view changes are 0.4 s spring blends of position, rotation and FOV.

**Shutter / photo mode**: pressing the in-game shutter freezes the local world
(enemies, particles), eases into a bullet-time orbit around W8I (drag to
orbit, pinch to dolly, slight horizon tilt), then offers:

- **Still**: path-traced from the current view, progress ring, cancellable.
- **Panorama**: 360 equirectangular (2:1) from W8I's position.

Output saves to the device; when played from a WUW CAM it uploads to that
camera's SD gallery (needs a firmware endpoint; until then, local save only).
Leaving photo mode resumes exactly where it froze. Freeze is local: peers keep
playing and are shown at their last received positions.

```
game state -> world.js / lighting.js -> realtime render (tier) -> screen
                                    |
         shutter -> freeze state ---+-> photo.js -> path tracer -> PNG / panorama -> save / gallery
```

## Requirements from sub-project G (shared world)

G will let up to **8 cameras** share one editable room (each camera's phone
plays as that camera), auto-discovered over ESP-NOW or the same WiFi, with a
host camera holding the authoritative world, automatic host migration
(lowest MAC), join snapshots, numbered edit operations, every camera keeping a
full copy on SD, and photos transferred over WiFi only. This renderer must:

- apply remote edits between frames, in order, ignoring stale or duplicate
  edits (incremental rebuild in `world.js`);
- draw up to 7 peer avatars (placeholder until B), fading after 1.5 s without
  updates;
- show a framed placeholder for photo textures not yet received.

## Failure handling

- Below target for ~2 s: lower internal resolution in steps to 0.5x, then
  disable light shafts -> GTAO -> shadow cascades -> bloom. Step back up with
  headroom.
- No WebGL2: load the existing `game-preview` renderer with a short note.
- Textures stream in after first frame (low-res first). Missing/corrupt asset:
  flat material of the right colour, never a crash.
- Path tracer unavailable: high-sample raster capture, labelled as such.
- WebGL context loss in photo mode: cancel, restore, offer retry.
- Gallery upload failure: keep the local download, retry once.

## Testing

- **Unit (Node)**: tier selection and degrade/upgrade order with simulated
  frame times; grid-to-modules and incremental rebuild scope; spring blends
  converge; wall-fade selection; remote edit ordering/dedup.
- **Browser (Playwright, desktop and 390x844)**: every view renders without
  console errors; blends not cuts; photo mode end-to-end at low samples
  produces a PNG; panorama is 2:1; screenshot comparisons per view.
- **Performance**: scripted walk on a fixed route logs average and 1%-low fps
  per tier. Must meet targets on the owner's Mac and on at least one real phone
  (manual, via an on-page fps readout).
- **Done**: all of the above pass and the owner signs off the screenshots.

## Licensing

Three.js and three-gpu-pathtracer are MIT; licenses ship in `site/bwo-hq/vendor/`.
PBR textures must be CC0 (for example ambientCG or Poly Haven) or original, so
the CC BY-SA 4.0 artwork licence stays clean; provenance is recorded in
`site/bwo-hq/assets/SOURCES.md`.
