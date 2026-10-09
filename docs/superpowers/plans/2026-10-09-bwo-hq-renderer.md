# BWO HQ Renderer + Cameras Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A drop-in photoreal Three.js renderer for BWO with adaptive quality tiers, walk/god/bird's-eye views, and a shutter-driven bullet-time photo mode that path-traces stills and panoramas.

**Architecture:** `site/bwo-hq/` holds ES modules bundled by esbuild into `hq.bundle.js`, which registers itself as `window.WuwShrineRenderer` exactly like the current `site/game-preview/rpg.bundle.js`. Pure logic (tier choice, frame governor, world planning, springs, wall-fade raycast) lives in Three-free modules unit-tested with `node --test`; Three-dependent modules are thin and verified in a browser with Playwright. The game page loads the HQ bundle when WebGL2 exists and falls back to the old bundle otherwise.

**Tech Stack:** Three.js 0.185.1 (+ `three/addons`), three-mesh-bvh 0.9.14, three-gpu-pathtracer 0.0.24, esbuild 0.25.10, Node 25 (`node --test`), @playwright/test 1.55.0.

Spec: `docs/superpowers/specs/2026-10-09-bwo-hq-renderer-design.md`.

---

## Ground rules (every task)

- Work in `/Users/Browny/Downloads/esp32cam-bwo` on branch `bwo-hq`. Never touch `site/game-preview/` except in Task 12.
- Commit messages are plain: no `Co-Authored-By` trailers, no mention of Claude/Anthropic/AI.
- All runtime code is bundled; no CDN URLs anywhere (the game must run offline on a camera's WiFi).
- Pure modules (`*.js` without `three` imports) must not import Three, so they run under `node --test`.
- `npm test` must stay green after every task.

## The game-state contract (read this first)

`site/game-preview/index.html` builds a state object `s` and calls the renderer each frame. The HQ renderer must accept exactly this shape:

| Field | Type | Meaning |
|---|---|---|
| `x`, `y` | number | player position in cells (`y` is the world Z axis) |
| `angle` | number | facing, radians; forward = `(cos a, sin a)` |
| `level` | int 0..2 | current level |
| `theme` | int 0..3 | palette/material theme |
| `uv` | int 0..3 | quarter-turn of floor/ceiling textures |
| `version` | int | bumps when the world changes |
| `world[level][z][x]` | byte | 0 empty; low nibble = wall tile; bit 7 (`128`) = placed cube block |
| `materials[level][z][x][face]` | int | photo code per face (0 = none). Face index: 0 = -Z, 1 = +X, 2 = +Z, 3 = -X, 4 = bottom, 5 = top (from `rpg-renderer.js`, whose Three.js box order +X,-X,+Y,-Y,+Z,-Z reads faces `[1,3,5,4,2,0]`) |
| `photos` | HTMLCanvasElement[] | photo slots; code `c` uses `photos[(c-1) % photos.length]` |
| `entities` | `[x, y, kind, active, level][]` | kind 0 crystal, 1 hostile organ |
| `moving`, `building` | bool | player walking; build cursor visible |
| `glitch` | number | glitch strength (0 = off) |
| `peers` (new, optional) | `{id, x, y, angle, level, age}[]` | other cameras' players; `age` seconds since last update |

The constructor is `new WuwShrineRenderer(hostElement, artCanvases)` where `artCanvases` are 64x64 tile canvases (index 0-1 player idle/step, 2-3 guardian, 4 crystal, 7 portrait, 8-15 wall/floor tiles, +16/+32 theme variants). The page sets `renderer.view = 'walk'` and reads `renderer.toggleView()`'s return value. After construction the renderer must `host.prepend()` a canvas with id `rpg-scene`.

## File structure

| Path | Responsibility | Three? |
|---|---|---|
| `site/bwo-hq/package.json` | pinned deps, `build`, `test`, `e2e` scripts | - |
| `site/bwo-hq/src/quality.js` | `pickTier(info)`, `Governor` (degrade/upgrade steps) | no |
| `site/bwo-hq/src/world-plan.js` | `planChunks(s)`, `chunkOf`, `changedChunks(prev, next)` | no |
| `site/bwo-hq/src/camera-math.js` | `Spring`, `viewPose(view, s, orbit)`, `wallsBetween(grid, from, to)` | no |
| `site/bwo-hq/src/materials.js` | procedural PBR textures per theme, photo frame + placeholder materials | yes |
| `site/bwo-hq/src/world.js` | chunk meshes from `planChunks`, incremental rebuild, wall fade | yes |
| `site/bwo-hq/src/lighting.js` | environment, sun + CSM, lanterns, per-tier setup | yes |
| `site/bwo-hq/src/post.js` | EffectComposer chain per tier, glitch pass | yes |
| `site/bwo-hq/src/cameras.js` | camera rigs, input, spring transitions, freeze/orbit | yes |
| `site/bwo-hq/src/actors.js` | placeholder W8I, entities, peers, build cursor | yes |
| `site/bwo-hq/src/photo.js` | path-traced still / panorama, raster fallback, PNG save | yes |
| `site/bwo-hq/src/hud.js` | view + shutter buttons, photo progress ring, fps readout | DOM |
| `site/bwo-hq/src/renderer.js` | `HQRenderer`: the drop-in class tying it together | yes |
| `site/bwo-hq/src/main.js` | bundle entry: registers `window.WuwShrineRenderer` | yes |
| `site/bwo-hq/test/*.test.js` | unit tests for pure modules | no |
| `site/bwo-hq/e2e/*.spec.js` | Playwright checks | - |
| `site/bwo-hq/hq.bundle.js` | build output (committed, like rpg.bundle.js) | - |
| `site/bwo-hq/vendor/LICENSES.md` | third-party licences | - |

---

### Task 1: Package scaffold (needs owner approval for npm downloads)

**Files:** Create `site/bwo-hq/package.json`, `site/bwo-hq/.gitignore`, `site/bwo-hq/src/main.js`, `site/bwo-hq/test/smoke.test.js`, `site/bwo-hq/vendor/LICENSES.md`

- [ ] **Step 1: package.json**

```json
{
  "name": "bwo-hq",
  "private": true,
  "type": "module",
  "license": "GPL-3.0-only",
  "scripts": {
    "build": "esbuild src/main.js --bundle --format=iife --minify --target=es2020 --outfile=hq.bundle.js",
    "test": "node --test test/",
    "e2e": "playwright test"
  },
  "dependencies": {
    "three": "0.185.1",
    "three-mesh-bvh": "0.9.14",
    "three-gpu-pathtracer": "0.0.24"
  },
  "devDependencies": {
    "esbuild": "0.25.10",
    "@playwright/test": "1.55.0"
  }
}
```

- [ ] **Step 2: `.gitignore`**

```gitignore
node_modules/
test-results/
playwright-report/
```

- [ ] **Step 3: install** (owner approval required: downloads the five packages above)

Run: `cd site/bwo-hq && npm install --no-audit --no-fund`
Expected: installs without errors. If npm reports an unmet peer (`xatlas-web`), run `npm install --no-audit --no-fund xatlas-web@0.1.0` (it is a peer of the path tracer, used only for lightmap baking which we do not use).

- [ ] **Step 4: smoke entry and test**

`src/main.js`:

```js
import { REVISION } from 'three';
export const THREE_REVISION = REVISION;
```

`test/smoke.test.js`:

```js
import test from 'node:test';
import assert from 'node:assert/strict';

test('node test runner works', () => {
  assert.equal(1 + 1, 2);
});
```

Run: `npm test && npm run build && ls -l hq.bundle.js`
Expected: 1 test passes; `hq.bundle.js` exists.

- [ ] **Step 5: licences**

`vendor/LICENSES.md`:

```markdown
# Third-party licences bundled into hq.bundle.js

| Package | Version | Licence |
|---|---|---|
| three | 0.185.1 | MIT, Copyright 2010-2025 three.js authors |
| three-mesh-bvh | 0.9.14 | MIT, Copyright Garrett Johnson |
| three-gpu-pathtracer | 0.0.24 | MIT, Copyright Garrett Johnson |

Full texts: `node_modules/<package>/LICENSE`, copied below by Step 6.
```

Then append each full licence: `for p in three three-mesh-bvh three-gpu-pathtracer; do printf '\n## %s\n\n```\n' $p >> vendor/LICENSES.md; cat node_modules/$p/LICENSE >> vendor/LICENSES.md; printf '```\n' >> vendor/LICENSES.md; done`

- [ ] **Step 6: Commit**

```bash
git add site/bwo-hq/package.json site/bwo-hq/package-lock.json site/bwo-hq/.gitignore site/bwo-hq/src/main.js site/bwo-hq/test/smoke.test.js site/bwo-hq/vendor/LICENSES.md site/bwo-hq/hq.bundle.js
git commit -m "BWO HQ: package scaffold with pinned Three.js and path tracer"
```

---

### Task 2: Quality tiers and frame governor (pure)

**Files:** Create `site/bwo-hq/src/quality.js`, `site/bwo-hq/test/quality.test.js`

- [ ] **Step 1: Failing tests** `test/quality.test.js`:

```js
import test from 'node:test';
import assert from 'node:assert/strict';
import { pickTier, Governor, TIERS, STEPS } from '../src/quality.js';

test('mobile GPUs pick the phone tier', () => {
  assert.equal(pickTier({ gpu: 'Apple GPU', mobile: true, benchMs: 9 }), 'phone');
  assert.equal(pickTier({ gpu: 'Adreno (TM) 740', mobile: true, benchMs: 6 }), 'phone');
});

test('a fast desktop picks the desktop tier', () => {
  assert.equal(pickTier({ gpu: 'Apple M2 Pro', mobile: false, benchMs: 4 }), 'desktop');
});

test('a slow desktop falls back to phone', () => {
  assert.equal(pickTier({ gpu: 'Intel(R) UHD Graphics 620', mobile: false, benchMs: 22 }), 'phone');
});

test('tier table matches the spec', () => {
  assert.equal(TIERS.phone.targetFps, 30);
  assert.equal(TIERS.phone.scale, 0.75);
  assert.equal(TIERS.desktop.targetFps, 60);
  assert.equal(TIERS.desktop.scale, 1);
  assert.deepEqual(STEPS, ['scale', 'shafts', 'gtao', 'cascades', 'bloom']);
});

function feed(g, ms, seconds) {
  const out = [];
  for (let t = 0; t < seconds * 1000; t += ms) { const a = g.frame(ms); if (a) out.push(a); }
  return out;
}

test('sustained slow frames lower resolution first, then effects in order', () => {
  const g = new Governor('desktop');
  const actions = feed(g, 25, 30); // 40 fps against a 60 target
  assert.deepEqual(actions.slice(0, 3).map(a => a.kind), ['scale', 'scale', 'scale']);
  assert.ok(g.state.scale >= 0.5);
  const offs = actions.filter(a => a.kind === 'off').map(a => a.what);
  assert.deepEqual(offs, ['shafts', 'gtao', 'cascades', 'bloom'].slice(0, offs.length));
});

test('no action within the 2 s grace window', () => {
  const g = new Governor('desktop');
  assert.deepEqual(feed(g, 25, 1.9), []);
});

test('headroom steps back up', () => {
  const g = new Governor('phone');
  feed(g, 50, 10);                 // 20 fps: degrade
  const low = g.state.scale;
  const ups = feed(g, 10, 20);     // 100 fps: recover
  assert.ok(ups.some(a => a.kind === 'scale' && a.to > low));
});
```

- [ ] **Step 2: Run, verify failure**

Run: `cd site/bwo-hq && npm test`
Expected: FAIL, cannot find `../src/quality.js`.

- [ ] **Step 3: Implement** `src/quality.js`:

```js
// Tier choice at startup and a frame governor that trades quality for
// smoothness during play. Pure: no Three.js, unit-tested under node.

export const TIERS = {
  phone:   { targetFps: 30, scale: 0.75, cascades: 1, gtao: false, shafts: false, bloom: true, aa: 'fxaa' },
  desktop: { targetFps: 60, scale: 1,    cascades: 3, gtao: true,  shafts: true,  bloom: true, aa: 'smaa' },
};

// Degrade order from the spec: resolution first, then shafts -> GTAO -> cascades -> bloom.
export const STEPS = ['scale', 'shafts', 'gtao', 'cascades', 'bloom'];

const MOBILE_GPU = /apple gpu|adreno|mali|powervr|xclipse|immortalis/i;

export function pickTier({ gpu = '', mobile = false, benchMs = 16 }) {
  if (mobile || MOBILE_GPU.test(gpu)) return 'phone';
  return benchMs <= 12 ? 'desktop' : 'phone';
}

const MIN_SCALE = 0.5;
const SCALE_STEP = 0.1;
const GRACE_MS = 2000;

export class Governor {
  constructor(tier) {
    const t = TIERS[tier];
    this.target = 1000 / t.targetFps;
    this.state = { scale: t.scale, shafts: t.shafts, gtao: t.gtao, cascades: t.cascades, bloom: t.bloom };
    this.base = { ...this.state };
    this.slowMs = 0;
    this.fastMs = 0;
  }

  // Feed one frame time; returns an action or null.
  frame(ms) {
    if (ms > this.target * 1.1) { this.slowMs += ms; this.fastMs = 0; }
    else if (ms < this.target * 0.7) { this.fastMs += ms; this.slowMs = 0; }
    else { this.slowMs = 0; this.fastMs = 0; }

    if (this.slowMs >= GRACE_MS) { this.slowMs = 0; return this.degrade(); }
    if (this.fastMs >= GRACE_MS * 2) { this.fastMs = 0; return this.upgrade(); }
    return null;
  }

  degrade() {
    const s = this.state;
    if (s.scale > MIN_SCALE + 1e-9) {
      s.scale = Math.max(MIN_SCALE, +(s.scale - SCALE_STEP).toFixed(2));
      return { kind: 'scale', to: s.scale };
    }
    for (const what of STEPS.slice(1)) {
      if (what === 'cascades' ? s.cascades > 1 : s[what]) {
        if (what === 'cascades') s.cascades = 1; else s[what] = false;
        return { kind: 'off', what };
      }
    }
    return null;
  }

  upgrade() {
    const s = this.state, b = this.base;
    for (const what of [...STEPS.slice(1)].reverse()) {
      if (what === 'cascades' ? s.cascades < b.cascades : b[what] && !s[what]) {
        if (what === 'cascades') s.cascades = b.cascades; else s[what] = true;
        return { kind: 'on', what };
      }
    }
    if (s.scale < b.scale - 1e-9) {
      s.scale = Math.min(b.scale, +(s.scale + SCALE_STEP).toFixed(2));
      return { kind: 'scale', to: s.scale };
    }
    return null;
  }
}
```

- [ ] **Step 4: Run tests** — `npm test`. Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add site/bwo-hq/src/quality.js site/bwo-hq/test/quality.test.js
git commit -m "BWO HQ: quality tiers and frame governor"
```

---

### Task 3: World planning and change detection (pure)

**Files:** Create `site/bwo-hq/src/world-plan.js`, `site/bwo-hq/test/world-plan.test.js`

The world is 24x24 per level, split into 6x6-cell chunks (16 per level). A chunk is the unit of rebuild, so an edit rebuilds at most a few chunks instead of the level.

- [ ] **Step 1: Failing tests** `test/world-plan.test.js`:

```js
import test from 'node:test';
import assert from 'node:assert/strict';
import { planChunk, chunkOf, changedChunks, CHUNK, SIZE } from '../src/world-plan.js';

function emptyState() {
  const lvl = () => Array.from({ length: SIZE }, () => Array(SIZE).fill(0));
  const mats = () => Array.from({ length: SIZE }, () => Array.from({ length: SIZE }, () => [0, 0, 0, 0, 0, 0]));
  return { level: 0, theme: 0, world: [lvl(), lvl(), lvl()], materials: [mats(), mats(), mats()] };
}

test('chunk geometry', () => {
  assert.equal(CHUNK, 6);
  assert.equal(SIZE, 24);
  assert.deepEqual(chunkOf(0, 0), 0);
  assert.deepEqual(chunkOf(23, 23), 15);
  assert.deepEqual(chunkOf(7, 13), 2 * 4 + 1);
});

test('a wall cell becomes a wall module with its region', () => {
  const s = emptyState();
  s.world[0][1][2] = 9;            // tile 9 at x=2, z=1
  const plan = planChunk(s, 0);
  assert.equal(plan.walls.length, 1);
  assert.deepEqual(plan.walls[0], { x: 2, z: 1, region: 0, cube: false, faces: [0, 0, 0, 0, 0, 0], tile: 9 });
});

test('cube blocks carry their face photos', () => {
  const s = emptyState();
  s.world[0][3][3] = 128 | 2;
  s.materials[0][3][3] = [0, 5, 0, 0, 0, 1];
  const w = planChunk(s, 0).walls[0];
  assert.equal(w.cube, true);
  assert.deepEqual(w.faces, [0, 5, 0, 0, 0, 1]);
});

test('empty cells become floor and ceiling tiles', () => {
  const s = emptyState();
  const plan = planChunk(s, 0);
  assert.equal(plan.floors.length, 36);
  assert.equal(plan.ceilings.length, 36);
});

test('editing one cell changes only its chunk', () => {
  const a = emptyState(), b = emptyState();
  b.world[0][13][7] = 1;
  assert.deepEqual([...changedChunks(a, b)], [chunkOf(7, 13)]);
});

test('a face photo change on a chunk edge also dirties the neighbour', () => {
  const a = emptyState(), b = emptyState();
  b.materials[0][2][5] = [7, 0, 0, 0, 0, 0];
  assert.deepEqual([...changedChunks(a, b)].sort((p, q) => p - q), [0, 1]);
});

test('level or theme change dirties everything', () => {
  const a = emptyState(), b = emptyState();
  b.theme = 2;
  assert.equal(changedChunks(a, b).size, 16);
});
```

- [ ] **Step 2: Run, verify failure** — `npm test`, expect module-not-found.

- [ ] **Step 3: Implement** `src/world-plan.js`:

```js
// Turn the BWO grid into per-chunk module lists, and find which chunks an
// edit touched. Pure: no Three.js.

export const SIZE = 24;
export const CHUNK = 6;
const PER_ROW = SIZE / CHUNK;

export function chunkOf(x, z) {
  return Math.floor(z / CHUNK) * PER_ROW + Math.floor(x / CHUNK);
}

function regionOf(x, z, level, theme) {
  return (Math.floor(x / 6) + Math.floor(z / 6) + level + (theme === 2 ? 1 : 0)) & 3;
}

export function planChunk(s, chunk) {
  const cx = (chunk % PER_ROW) * CHUNK, cz = Math.floor(chunk / PER_ROW) * CHUNK;
  const map = s.world[s.level], faces = s.materials[s.level];
  const walls = [], floors = [], ceilings = [];
  for (let z = cz; z < cz + CHUNK; z++) {
    for (let x = cx; x < cx + CHUNK; x++) {
      const cell = map[z][x];
      if (cell) {
        walls.push({ x, z, region: regionOf(x, z, s.level, s.theme || 0), cube: !!(cell & 128),
                     faces: [...faces[z][x]], tile: cell & 15 });
      } else {
        floors.push({ x, z, photo: faces[z][x][4] });
        ceilings.push({ x, z, photo: faces[z][x][5] });
      }
    }
  }
  return { chunk, walls, floors, ceilings };
}

function cellKey(s, x, z) {
  return s.world[s.level][z][x] + ':' + s.materials[s.level][z][x].join(',');
}

// Chunks to rebuild going from state a to state b. A changed edge cell also
// dirties neighbours, because bevels and trims are shared across the seam.
export function changedChunks(a, b) {
  const all = new Set(Array.from({ length: PER_ROW * PER_ROW }, (_, i) => i));
  if (!a || a.level !== b.level || (a.theme || 0) !== (b.theme || 0)) return all;
  const out = new Set();
  for (let z = 0; z < SIZE; z++) {
    for (let x = 0; x < SIZE; x++) {
      if (cellKey(a, x, z) === cellKey(b, x, z)) continue;
      for (const [dx, dz] of [[0, 0], [1, 0], [-1, 0], [0, 1], [0, -1]]) {
        const nx = x + dx, nz = z + dz;
        if (nx >= 0 && nz >= 0 && nx < SIZE && nz < SIZE) out.add(chunkOf(nx, nz));
      }
    }
  }
  return out;
}

// Deep-enough copy of the parts changedChunks compares.
export function snapshot(s) {
  return {
    level: s.level, theme: s.theme || 0,
    world: s.world.map(l => l.map(r => [...r])),
    materials: s.materials.map(l => l.map(r => r.map(f => [...f]))),
  };
}
```

- [ ] **Step 4: Run tests** — `npm test`, all pass.

- [ ] **Step 5: Commit**

```bash
git add site/bwo-hq/src/world-plan.js site/bwo-hq/test/world-plan.test.js
git commit -m "BWO HQ: chunked world planning and change detection"
```

---

### Task 4: Camera math (pure)

**Files:** Create `site/bwo-hq/src/camera-math.js`, `site/bwo-hq/test/camera-math.test.js`

- [ ] **Step 1: Failing tests** `test/camera-math.test.js`:

```js
import test from 'node:test';
import assert from 'node:assert/strict';
import { Spring, viewPose, wallsBetween, VIEWS, BLEND_S } from '../src/camera-math.js';

test('views and blend time', () => {
  assert.deepEqual(VIEWS, ['walk', 'god', 'bird']);
  assert.equal(BLEND_S, 0.4);
});

test('spring settles on its target within the blend time', () => {
  const s = new Spring(0, BLEND_S);
  s.target = 10;
  for (let i = 0; i < 60; i++) s.step(1 / 60);   // 1 s
  assert.ok(Math.abs(s.value - 10) < 0.05);
});

test('spring is 90% there at the blend time', () => {
  const s = new Spring(0, BLEND_S);
  s.target = 1;
  for (let i = 0; i < 24; i++) s.step(1 / 60);   // 0.4 s
  assert.ok(s.value > 0.9 && s.value <= 1.001);
});

const st = { x: 5.5, y: 5.5, angle: 0 };

test('walk view sits at eye height looking forward', () => {
  const p = viewPose('walk', st, {});
  assert.deepEqual(p.pos.map(v => +v.toFixed(2)), [5.5, 0.63, 5.5]);
  assert.ok(p.look[0] > 5.5);
});

test('god view is high and behind, honouring orbit and zoom', () => {
  const p = viewPose('god', st, { yaw: 0, zoom: 1 });
  assert.ok(p.pos[1] > 4);
  const near = viewPose('god', st, { yaw: 0, zoom: 0.2 });
  assert.ok(near.pos[1] < p.pos[1]);
});

test('bird view is straight above the player', () => {
  const p = viewPose('bird', st, {});
  assert.equal(+p.pos[0].toFixed(2), 5.5);
  assert.equal(+p.pos[2].toFixed(2), 5.5);
  assert.ok(p.pos[1] > 12);
});

test('wallsBetween finds the cells crossed by the sight line', () => {
  const grid = Array.from({ length: 24 }, () => Array(24).fill(0));
  grid[5][7] = 1; grid[5][9] = 3;
  const hit = wallsBetween(grid, [10.5, 5.5], [5.5, 5.5]);
  assert.deepEqual(hit.map(([x, z]) => `${x},${z}`).sort(), ['7,5', '9,5']);
});

test('wallsBetween ignores the player and camera cells', () => {
  const grid = Array.from({ length: 24 }, () => Array(24).fill(1));
  assert.equal(wallsBetween(grid, [3.5, 3.5], [3.6, 3.6]).length, 0);
});
```

- [ ] **Step 2: Run, verify failure.**

- [ ] **Step 3: Implement** `src/camera-math.js`:

```js
// Camera poses, springs and line-of-sight for wall fading. Pure.

export const VIEWS = ['walk', 'god', 'bird'];
export const BLEND_S = 0.4;

// Critically damped spring: ~90% of the way to target after `settle` seconds.
export class Spring {
  constructor(value, settle = BLEND_S) {
    this.value = value; this.target = value; this.vel = 0;
    this.omega = 3.9 / settle;   // ln-based: (1 + wt) e^-wt ~= 0.1 at wt = 3.89
  }
  step(dt) {
    const w = this.omega, x = this.value - this.target;
    const e = Math.exp(-w * dt);
    const v = this.vel;
    this.value = this.target + (x + (v + w * x) * dt) * e;
    this.vel = (v - w * (v + w * x) * dt) * e;
    return this.value;
  }
}

// pos/look are [x, y, z] in world units; game y maps to world z.
export function viewPose(view, s, { yaw = 0, zoom = 1 } = {}) {
  const fx = Math.cos(s.angle), fz = Math.sin(s.angle);
  if (view === 'walk') {
    return { pos: [s.x, 0.63, s.y], look: [s.x + fx, 0.61, s.y + fz], fov: 62 };
  }
  if (view === 'bird') {
    return { pos: [s.x, 14 * Math.max(0.4, zoom), s.y + 0.001], look: [s.x, 0, s.y], fov: 50 };
  }
  // god: high three-quarter behind the player, orbited by yaw, zoom 0.2..1.
  const dist = 2 + 8 * zoom, height = 1.4 + 6 * zoom;
  const a = s.angle + Math.PI + yaw;
  return { pos: [s.x + Math.cos(a) * dist, height, s.y + Math.sin(a) * dist],
           look: [s.x, 0.6, s.y], fov: 55 };
}

// Cells with walls strictly between camera (from) and player (to), by 2D DDA.
export function wallsBetween(grid, [x0, z0], [x1, z1]) {
  const out = [];
  let cx = Math.floor(x0), cz = Math.floor(z0);
  const ex = Math.floor(x1), ez = Math.floor(z1);
  const dx = x1 - x0, dz = z1 - z0;
  const sx = Math.sign(dx), sz = Math.sign(dz);
  const tdx = dx ? Math.abs(1 / dx) : Infinity, tdz = dz ? Math.abs(1 / dz) : Infinity;
  let tx = dx ? ((sx > 0 ? cx + 1 - x0 : x0 - cx) * tdx) : Infinity;
  let tz = dz ? ((sz > 0 ? cz + 1 - z0 : z0 - cz) * tdz) : Infinity;
  for (let i = 0; i < 64 && !(cx === ex && cz === ez); i++) {
    if (tx < tz) { cx += sx; tx += tdx; } else { cz += sz; tz += tdz; }
    if (cx === ex && cz === ez) break;
    if (cz >= 0 && cx >= 0 && cz < grid.length && cx < grid[0].length && grid[cz][cx]) out.push([cx, cz]);
  }
  return out;
}
```

- [ ] **Step 4: Run tests** — all pass. If the spring 90% test fails, tune only `omega`'s constant and note the value.

- [ ] **Step 5: Commit**

```bash
git add site/bwo-hq/src/camera-math.js site/bwo-hq/test/camera-math.test.js
git commit -m "BWO HQ: camera poses, springs and line-of-sight"
```

---

### Task 5: Procedural PBR materials

**Files:** Create `site/bwo-hq/src/materials.js`

Procedural textures keep the first version free of downloads; Task 14 swaps in CC0 scans.

- [ ] **Step 1: Implement** `src/materials.js`:

```js
import * as THREE from 'three';

// Theme base colours (albedo) for the four wall regions, floor, ceiling.
const THEMES = [
  { walls: [0xd8d0bf, 0x5f8f7c, 0x8e2f2a, 0x9a9da0], floor: 0x6f776b, ceil: 0xe2dccb },
  { walls: [0xb8b3a6, 0x3f6b5e, 0x6e2423, 0x7d8285], floor: 0x575d55, ceil: 0xc9c3b3 },
  { walls: [0x9e8f86, 0x4c5f57, 0x7a1f26, 0x6a6466], floor: 0x4a3a33, ceil: 0xb5a99c },
  { walls: [0xe6e1d3, 0x7fa898, 0xa4473d, 0xb7babc], floor: 0x7d857b, ceil: 0xf0ebdc },
];
const ROUGH = [0.82, 0.38, 0.45, 0.55];   // stone, jade metal, glazed ceramic, machinery
const METAL = [0.0, 0.6, 0.0, 0.75];

function noiseCanvas(size, seed, fn) {
  const c = document.createElement('canvas'); c.width = c.height = size;
  const ctx = c.getContext('2d'), img = ctx.createImageData(size, size);
  let r = seed >>> 0;
  const rnd = () => ((r = (r * 1664525 + 1013904223) >>> 0) / 4294967296);
  for (let i = 0; i < size * size; i++) {
    const [a, b, cc] = fn(i % size, Math.floor(i / size), rnd);
    img.data.set([a, b, cc, 255], i * 4);
  }
  ctx.putImageData(img, 0, 0);
  return c;
}

function tex(canvas, srgb) {
  const t = new THREE.CanvasTexture(canvas);
  t.wrapS = t.wrapT = THREE.RepeatWrapping;
  t.anisotropy = 8;
  if (srgb) t.colorSpace = THREE.SRGBColorSpace;
  return t;
}

// Stone-block albedo, bumpy normal and roughness maps from one seed.
function pbrSet(hex, seed, rough) {
  const base = new THREE.Color(hex);
  const size = 256;
  const albedo = noiseCanvas(size, seed, (x, y, rnd) => {
    const mortar = (x % 128 < 3 || y % 64 < 3) ? 0.55 : 1;
    const n = 0.88 + rnd() * 0.24;
    return [base.r * 255 * n * mortar, base.g * 255 * n * mortar, base.b * 255 * n * mortar];
  });
  const normal = noiseCanvas(size, seed + 1, (x, y, rnd) => {
    const edgeX = x % 128 < 3 ? -60 : x % 128 > 124 ? 60 : 0;
    const edgeY = y % 64 < 3 ? -60 : y % 64 > 60 ? 60 : 0;
    return [128 + edgeX + (rnd() - 0.5) * 30, 128 + edgeY + (rnd() - 0.5) * 30, 255];
  });
  const roughMap = noiseCanvas(size, seed + 2, (x, y, rnd) => {
    const v = Math.min(255, Math.max(0, (rough + (rnd() - 0.5) * 0.15) * 255));
    return [v, v, v];
  });
  return { map: tex(albedo, true), normalMap: tex(normal, false), roughnessMap: tex(roughMap, false) };
}

export class MaterialLibrary {
  constructor() { this.cache = new Map(); this.photo = new Map(); }

  wall(theme, region) {
    const key = `w${theme}:${region}`;
    if (!this.cache.has(key)) {
      const set = pbrSet(THEMES[theme].walls[region], 11 + theme * 7 + region, ROUGH[region]);
      this.cache.set(key, new THREE.MeshStandardMaterial({ ...set, roughness: 1, metalness: METAL[region] }));
    }
    return this.cache.get(key);
  }

  floor(theme, ceiling) {
    const key = `${ceiling ? 'c' : 'f'}${theme}`;
    if (!this.cache.has(key)) {
      const set = pbrSet(ceiling ? THEMES[theme].ceil : THEMES[theme].floor, 101 + theme * 3 + (ceiling ? 1 : 0), ceiling ? 0.7 : 0.9);
      this.cache.set(key, new THREE.MeshStandardMaterial({ ...set, roughness: 1, metalness: 0 }));
    }
    return this.cache.get(key);
  }

  // A player photo as a softly self-lit panel. Missing canvas -> placeholder frame.
  photoMaterial(canvas) {
    if (!canvas) return this.placeholder();
    if (!this.photo.has(canvas)) {
      const map = new THREE.CanvasTexture(canvas);
      map.colorSpace = THREE.SRGBColorSpace;
      this.photo.set(canvas, new THREE.MeshStandardMaterial({
        map, emissiveMap: map, emissive: 0xffffff, emissiveIntensity: 0.35, roughness: 0.4, metalness: 0,
      }));
    }
    return this.photo.get(canvas);
  }

  placeholder() {
    if (!this.cache.has('placeholder')) {
      const c = noiseCanvas(64, 5, (x, y) => {
        const frame = x < 4 || y < 4 || x > 59 || y > 59;
        return frame ? [180, 160, 120] : [40, 44, 46];
      });
      this.cache.set('placeholder', new THREE.MeshStandardMaterial({ map: tex(c, true), roughness: 0.8 }));
    }
    return this.cache.get('placeholder');
  }

  // Drop photo materials whose canvas is no longer in the slot list.
  prunePhotos(canvases) {
    for (const [canvas, mat] of this.photo) {
      if (!canvases.includes(canvas)) { mat.map.dispose(); mat.dispose(); this.photo.delete(canvas); }
    }
  }
}
```

- [ ] **Step 2: Build check** — `npm run build` must succeed (main.js does not import this yet, so also run `npx esbuild src/materials.js --bundle --format=esm --outfile=/tmp/mat-check.js`). Expected: no errors.

- [ ] **Step 3: Commit**

```bash
git add site/bwo-hq/src/materials.js
git commit -m "BWO HQ: procedural PBR material library"
```

---

### Task 6: World meshes with incremental chunk rebuild and wall fade

**Files:** Create `site/bwo-hq/src/world.js`

- [ ] **Step 1: Implement** `src/world.js`:

```js
import * as THREE from 'three';
import { RoundedBoxGeometry } from 'three/addons/geometries/RoundedBoxGeometry.js';
import { planChunk, changedChunks, snapshot } from './world-plan.js';

const WALL_H = 1.4;
// Three.js box material order +X,-X,+Y,-Y,+Z,-Z maps to BWO face indices:
const BOX_FACE = [1, 3, 5, 4, 2, 0];

export class World {
  constructor(scene, materials) {
    this.scene = scene;
    this.mats = materials;
    this.root = new THREE.Group();
    scene.add(this.root);
    this.chunks = new Map();          // chunk id -> THREE.Group
    this.prev = null;
    this.wallGeo = new RoundedBoxGeometry(1, WALL_H, 1, 3, 0.045);
    this.cubeGeo = new RoundedBoxGeometry(1, 1, 1, 3, 0.04);
    this.tileGeo = new THREE.PlaneGeometry(1, 1);
    this.faded = new Set();           // "x,z" keys currently hidden
    this.ceilings = [];
  }

  photoFor(s, code) {
    if (!code) return null;
    return this.mats.photoMaterial(s.photos.length ? s.photos[(code - 1) % s.photos.length] : null);
  }

  // Rebuild only the chunks that changed since the last call. Returns count.
  update(s) {
    const dirty = changedChunks(this.prev, s);
    for (const id of dirty) this.buildChunk(s, id);
    if (dirty.size) this.prev = snapshot(s);
    return dirty.size;
  }

  forceRebuild(s) { this.prev = null; return this.update(s); }

  buildChunk(s, id) {
    const old = this.chunks.get(id);
    if (old) { this.root.remove(old); old.traverse(o => { if (o.isInstancedMesh) o.dispose(); }); }
    const plan = planChunk(s, id), g = new THREE.Group(), theme = s.theme || 0;
    g.userData.walls = new Map();     // "x,z" -> {mesh, index}

    const byRegion = [[], [], [], []];
    for (const w of plan.walls) {
      if (w.cube) {
        const base = this.mats.wall(theme, w.region);
        const mats = BOX_FACE.map(f => this.photoFor(s, w.faces[f]) || base);
        const m = new THREE.Mesh(this.cubeGeo, mats);
        m.position.set(w.x + 0.5, 0.5, w.z + 0.5);
        m.castShadow = m.receiveShadow = true;
        g.add(m);
        g.userData.walls.set(`${w.x},${w.z}`, { mesh: m });
      } else byRegion[w.region].push(w);
    }
    const t = new THREE.Object3D();
    byRegion.forEach((list, region) => {
      if (!list.length) return;
      const mesh = new THREE.InstancedMesh(this.wallGeo, this.mats.wall(theme, region), list.length);
      list.forEach((w, i) => {
        t.position.set(w.x + 0.5, WALL_H / 2, w.z + 0.5); t.scale.set(1, 1, 1); t.updateMatrix();
        mesh.setMatrixAt(i, t.matrix);
        g.userData.walls.set(`${w.x},${w.z}`, { mesh, index: i, matrix: t.matrix.clone() });
      });
      mesh.castShadow = mesh.receiveShadow = true;
      g.add(mesh);
    });

    for (const [list, y, ceiling] of [[plan.floors, 0, false], [plan.ceilings, WALL_H, true]]) {
      const plain = list.filter(c => !c.photo);
      if (plain.length) {
        const mesh = new THREE.InstancedMesh(this.tileGeo, this.mats.floor(theme, ceiling), plain.length);
        plain.forEach((c, i) => {
          t.position.set(c.x + 0.5, y, c.z + 0.5); t.rotation.set(ceiling ? Math.PI / 2 : -Math.PI / 2, 0, 0);
          t.scale.set(1, 1, 1); t.updateMatrix(); mesh.setMatrixAt(i, t.matrix);
        });
        mesh.receiveShadow = true;
        mesh.userData.ceiling = ceiling;
        g.add(mesh);
      }
      for (const c of list.filter(c => c.photo)) {
        const m = new THREE.Mesh(this.tileGeo, this.photoFor(s, c.photo));
        m.position.set(c.x + 0.5, y, c.z + 0.5);
        m.rotation.x = ceiling ? Math.PI / 2 : -Math.PI / 2;
        m.receiveShadow = true; m.userData.ceiling = ceiling;
        g.add(m);
      }
    }
    this.root.add(g);
    this.chunks.set(id, g);
    this.ceilings = [];
    this.root.traverse(o => { if (o.userData.ceiling) this.ceilings.push(o); });
    this.faded.clear();
  }

  showCeilings(on) { for (const o of this.ceilings) o.visible = on; }

  // Hide walls in `cells` ([[x,z],...]); restore the rest. Instanced walls are
  // hidden by zero scale, cube blocks by visibility.
  fade(cells) {
    const want = new Set(cells.map(([x, z]) => `${x},${z}`));
    const zero = new THREE.Matrix4().makeScale(0, 0, 0);
    const touched = new Set();
    for (const key of new Set([...want, ...this.faded])) {
      for (const g of this.chunks.values()) {
        const w = g.userData.walls.get(key);
        if (!w) continue;
        const hide = want.has(key);
        if (w.index === undefined) w.mesh.visible = !hide;
        else { w.mesh.setMatrixAt(w.index, hide ? zero : w.matrix); touched.add(w.mesh); }
      }
    }
    for (const m of touched) m.instanceMatrix.needsUpdate = true;
    this.faded = want;
  }
}
```

- [ ] **Step 2: Build check** — `npx esbuild src/world.js --bundle --format=esm --outfile=/tmp/world-check.js`. Expected: success.

- [ ] **Step 3: Commit**

```bash
git add site/bwo-hq/src/world.js
git commit -m "BWO HQ: bevelled chunk meshes, incremental rebuild, wall fade"
```

---

### Task 7: Lighting

**Files:** Create `site/bwo-hq/src/lighting.js`

- [ ] **Step 1: Implement** `src/lighting.js`:

```js
import * as THREE from 'three';
import { RoomEnvironment } from 'three/addons/environments/RoomEnvironment.js';
import { CSM } from 'three/addons/csm/CSM.js';

const FOG = [0x2a3031, 0x232c30, 0x33171f, 0x6b7f7a];

export class Lighting {
  constructor(renderer, scene, camera, tier) {
    this.scene = scene;
    const pmrem = new THREE.PMREMGenerator(renderer);
    scene.environment = pmrem.fromScene(new RoomEnvironment(), 0.04).texture;
    scene.environmentIntensity = 0.55;
    pmrem.dispose();

    scene.fog = new THREE.FogExp2(FOG[0], 0.045);
    scene.background = new THREE.Color(FOG[0]);

    this.hemi = new THREE.HemisphereLight(0xe6f3db, 0x3a3240, 0.35);
    scene.add(this.hemi);

    this.csm = new CSM({
      maxFar: 30, cascades: tier.cascades, mode: 'practical', parent: scene,
      shadowMapSize: tier.cascades > 1 ? 2048 : 1024, lightDirection: new THREE.Vector3(0.45, -1, 0.3).normalize(),
      lightIntensity: 2.4, camera,
    });
    this.csm.lights.forEach(l => { l.color.set(0xffe2b0); l.shadow.bias = -0.0004; l.shadow.normalBias = 0.02; });

    // Lanterns at the shrine pillars (every 6 cells), warm and short-range.
    this.lanterns = [];
    for (let z = 6; z < 24; z += 12) for (let x = 6; x < 24; x += 12) {
      const l = new THREE.PointLight(0xffb066, 6, 7, 2);
      l.position.set(x + 0.5, 1.15, z + 0.5);
      scene.add(l); this.lanterns.push(l);
    }
  }

  // Materials must be registered so CSM can inject its shader chunk.
  setupMaterial(mat) { this.csm.setupMaterial(mat); }

  setTheme(theme) {
    this.scene.fog.color.setHex(FOG[theme]);
    this.scene.background.setHex(FOG[theme]);
  }

  setCamera(camera) { this.csm.camera = camera; this.csm.updateFrustums(); }

  setCascades(n) {
    // CSM cannot change cascade count live; collapse extra cascades' shadows.
    this.csm.lights.forEach((l, i) => { l.castShadow = i < n; });
  }

  update() { this.csm.update(); }
}
```

- [ ] **Step 2: Build check** — `npx esbuild src/lighting.js --bundle --format=esm --outfile=/tmp/light-check.js`. If `three/addons/csm/CSM.js` does not exist in 0.185.1, check `ls node_modules/three/examples/jsm/csm/` and use the file there; report the change.

- [ ] **Step 3: Commit**

```bash
git add site/bwo-hq/src/lighting.js
git commit -m "BWO HQ: environment lighting, cascaded sun shadows, lanterns"
```

---

### Task 8: Post-processing chain

**Files:** Create `site/bwo-hq/src/post.js`

- [ ] **Step 1: Implement** `src/post.js`:

```js
import * as THREE from 'three';
import { EffectComposer } from 'three/addons/postprocessing/EffectComposer.js';
import { RenderPass } from 'three/addons/postprocessing/RenderPass.js';
import { GTAOPass } from 'three/addons/postprocessing/GTAOPass.js';
import { UnrealBloomPass } from 'three/addons/postprocessing/UnrealBloomPass.js';
import { SMAAPass } from 'three/addons/postprocessing/SMAAPass.js';
import { ShaderPass } from 'three/addons/postprocessing/ShaderPass.js';
import { OutputPass } from 'three/addons/postprocessing/OutputPass.js';
import { FXAAShader } from 'three/addons/shaders/FXAAShader.js';

// Port of the game-preview glitch: horizontal band tearing with an R shift.
const GlitchShader = {
  uniforms: { tDiffuse: { value: null }, time: { value: 0 }, strength: { value: 0 } },
  vertexShader: 'varying vec2 vUv; void main(){ vUv = uv; gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0); }',
  fragmentShader: `uniform sampler2D tDiffuse; uniform float time; uniform float strength; varying vec2 vUv;
    void main(){
      float band = step(.96, fract(vUv.y * 17. + floor(time * 5.) * .137));
      vec2 p = vec2(clamp(vUv.x + band * strength * .004, 0., 1.), vUv.y);
      vec4 c = texture2D(tDiffuse, p);
      c.r = texture2D(tDiffuse, vec2(clamp(p.x + band * strength * .002, 0., 1.), p.y)).r;
      gl_FragColor = c;
    }`,
};

export class Post {
  constructor(renderer, scene, camera, tier) {
    this.renderer = renderer;
    this.composer = new EffectComposer(renderer);
    this.render = new RenderPass(scene, camera);
    this.composer.addPass(this.render);
    this.gtao = new GTAOPass(scene, camera, 1, 1);
    this.gtao.enabled = tier.gtao;
    this.composer.addPass(this.gtao);
    this.bloom = new UnrealBloomPass(new THREE.Vector2(1, 1), 0.35, 0.6, 0.85);
    this.bloom.enabled = tier.bloom;
    this.composer.addPass(this.bloom);
    this.glitch = new ShaderPass(GlitchShader);
    this.glitch.enabled = false;
    this.composer.addPass(this.glitch);
    this.composer.addPass(new OutputPass());
    if (tier.aa === 'smaa') { this.aa = new SMAAPass(1, 1); }
    else { this.aa = new ShaderPass(FXAAShader); this.fxaa = true; }
    this.composer.addPass(this.aa);
  }

  setCamera(camera) { this.render.camera = camera; this.gtao.camera = camera; }

  setSize(w, h) {
    this.composer.setSize(w, h);
    if (this.fxaa) this.aa.material.uniforms.resolution.value.set(1 / w, 1 / h);
  }

  set(what, on) { if (this[what]) this[what].enabled = on; }

  frame(t, glitch) {
    this.glitch.enabled = glitch > 0;
    this.glitch.uniforms.time.value = t / 1000;
    this.glitch.uniforms.strength.value = glitch;
    this.composer.render();
  }
}
```

- [ ] **Step 2: Build check** — `npx esbuild src/post.js --bundle --format=esm --outfile=/tmp/post-check.js`. Fix import paths against `node_modules/three/examples/jsm/` if any moved; report changes.

- [ ] **Step 3: Commit**

```bash
git add site/bwo-hq/src/post.js
git commit -m "BWO HQ: post-processing chain with tier toggles and glitch pass"
```

---

### Task 9: Actors (placeholder W8I, entities, peers, cursor)

**Files:** Create `site/bwo-hq/src/actors.js`

- [ ] **Step 1: Implement** `src/actors.js`:

```js
import * as THREE from 'three';

const PEER_FADE_S = 1.5;
const MAX_PEERS = 7;

// Capsule body + head + crimson hair cap: stands in for W8I until sub-project B.
function figure(hair = 0xb3121f, coat = 0xb9bcbf) {
  const g = new THREE.Group();
  const body = new THREE.Mesh(new THREE.CapsuleGeometry(0.14, 0.42, 6, 12),
    new THREE.MeshStandardMaterial({ color: coat, roughness: 0.7 }));
  body.position.y = 0.37;
  const head = new THREE.Mesh(new THREE.SphereGeometry(0.11, 20, 16),
    new THREE.MeshStandardMaterial({ color: 0xa8714f, roughness: 0.55 }));
  head.position.y = 0.78;
  const hairCap = new THREE.Mesh(new THREE.SphereGeometry(0.125, 20, 16, 0, Math.PI * 2, 0, Math.PI * 0.62),
    new THREE.MeshStandardMaterial({ color: hair, roughness: 0.45 }));
  hairCap.position.y = 0.8;
  for (const m of [body, head, hairCap]) { m.castShadow = true; g.add(m); }
  return g;
}

export class Actors {
  constructor(scene) {
    this.scene = scene;
    this.player = figure();
    scene.add(this.player);
    this.entities = [];
    this.peers = new Map();
    this.crystalGeo = new THREE.OctahedronGeometry(0.16, 0);
    this.crystalMat = new THREE.MeshPhysicalMaterial({ color: 0x6fd6b8, roughness: 0.05, transmission: 0.6, thickness: 0.3, emissive: 0x1d6b55, emissiveIntensity: 0.6 });
    this.organGeo = new THREE.IcosahedronGeometry(0.32, 2);
    this.organMat = new THREE.MeshStandardMaterial({ color: 0x7a1a24, roughness: 0.35, metalness: 0.1 });
    this.cursor = new THREE.Mesh(new THREE.PlaneGeometry(0.91, 0.91),
      new THREE.MeshBasicMaterial({ color: 0xf3c26a, transparent: true, opacity: 0.35, depthWrite: false, side: THREE.DoubleSide }));
    this.cursor.rotation.x = -Math.PI / 2;
    scene.add(this.cursor);
  }

  update(s, t, view) {
    this.player.visible = view !== 'walk';
    this.player.position.set(s.x, 0, s.y);
    this.player.rotation.y = -s.angle + Math.PI / 2;
    this.player.position.y = s.moving ? Math.abs(Math.sin(t * 0.012)) * 0.03 : 0;

    s.entities.forEach((e, i) => {
      let m = this.entities[i];
      const kind = e[2] ? 'organ' : 'crystal';
      if (!m || m.userData.kind !== kind) {
        if (m) this.scene.remove(m);
        m = new THREE.Mesh(e[2] ? this.organGeo : this.crystalGeo, e[2] ? this.organMat : this.crystalMat);
        m.castShadow = true; m.userData.kind = kind;
        this.scene.add(m); this.entities[i] = m;
      }
      m.visible = !!e[3] && e[4] === s.level;
      const bob = e[2] ? 0 : 0.1 + Math.sin(t * 0.003 + i) * 0.045;
      m.position.set(e[0], (e[2] ? 0.34 : 0.45) + bob, e[1]);
      m.rotation.y = t * (e[2] ? 0.0004 : 0.0012) + i;
      if (e[2]) m.scale.setScalar(1 + Math.sin(t * 0.004 + i) * 0.04);
    });
    for (let i = s.entities.length; i < this.entities.length; i++) this.entities[i].visible = false;

    this.cursor.visible = !!s.building;
    this.cursor.position.set(Math.floor(s.x + Math.cos(s.angle) * 1.5) + 0.5, 0.018, Math.floor(s.y + Math.sin(s.angle) * 1.5) + 0.5);

    this.updatePeers(s.peers || [], s.level);
  }

  updatePeers(list, level) {
    const seen = new Set();
    for (const p of list.slice(0, MAX_PEERS)) {
      seen.add(p.id);
      let g = this.peers.get(p.id);
      if (!g) {
        g = figure(0x2f7fd1, 0x8a8f93);
        g.traverse(o => { if (o.material) { o.material = o.material.clone(); o.material.transparent = true; } });
        this.scene.add(g); this.peers.set(p.id, g);
      }
      const alpha = Math.max(0, 1 - Math.max(0, p.age - PEER_FADE_S) / 0.5);
      g.visible = p.level === level && alpha > 0;
      g.position.set(p.x, 0, p.y);
      g.rotation.y = -p.angle + Math.PI / 2;
      g.traverse(o => { if (o.material) o.material.opacity = alpha; });
    }
    for (const [id, g] of this.peers) if (!seen.has(id)) { this.scene.remove(g); this.peers.delete(id); }
  }
}
```

Note on peer fade: the spec says peers fade after 1.5 s without updates; this fades from 1.5 s to 2.0 s then hides.

- [ ] **Step 2: Build check** — `npx esbuild src/actors.js --bundle --format=esm --outfile=/tmp/actors-check.js`.

- [ ] **Step 3: Commit**

```bash
git add site/bwo-hq/src/actors.js
git commit -m "BWO HQ: placeholder W8I, PBR entities, peer avatars, build cursor"
```

---

### Task 10: Cameras, input and freeze/orbit

**Files:** Create `site/bwo-hq/src/cameras.js`

- [ ] **Step 1: Implement** `src/cameras.js`:

```js
import * as THREE from 'three';
import { Spring, viewPose, wallsBetween, VIEWS, BLEND_S } from './camera-math.js';

export class Cameras {
  constructor(dom) {
    this.camera = new THREE.PerspectiveCamera(62, 16 / 9, 0.05, 60);
    this.view = 'walk';
    this.orbit = { yaw: 0, zoom: 0.6 };
    this.frozen = null;               // { x, y, angle } while in photo mode
    this.orbitFrozen = { yaw: 0, pitch: 0.25, dist: 2.2 };
    this.springs = { px: new Spring(0), py: new Spring(0), pz: new Spring(0),
                     lx: new Spring(0), ly: new Spring(0), lz: new Spring(0), fov: new Spring(62) };
    this.first = true;
    this.bindInput(dom);
  }

  setView(name) { if (VIEWS.includes(name)) this.view = name; return this.view; }
  cycle() { return this.setView(VIEWS[(VIEWS.indexOf(this.view) + 1) % VIEWS.length]); }

  freeze(s) { this.frozen = { x: s.x, y: s.y, angle: s.angle }; this.orbitFrozen = { yaw: 0, pitch: 0.25, dist: 2.2 }; }
  unfreeze() { this.frozen = null; }

  bindInput(dom) {
    let drag = null, pinch = null;
    dom.addEventListener('pointerdown', e => { drag = { x: e.clientX, y: e.clientY }; dom.setPointerCapture(e.pointerId); });
    dom.addEventListener('pointerup', () => { drag = null; });
    dom.addEventListener('pointermove', e => {
      if (!drag || pinch) return;
      const dx = (e.clientX - drag.x) / dom.clientWidth, dy = (e.clientY - drag.y) / dom.clientHeight;
      drag = { x: e.clientX, y: e.clientY };
      if (this.frozen) { this.orbitFrozen.yaw += dx * 4; this.orbitFrozen.pitch = Math.min(1.2, Math.max(-0.2, this.orbitFrozen.pitch + dy * 2)); }
      else if (this.view === 'god') this.orbit.yaw += dx * 4;
    });
    dom.addEventListener('wheel', e => { e.preventDefault(); this.zoomBy(e.deltaY * 0.001); }, { passive: false });
    dom.addEventListener('touchmove', e => {
      if (e.touches.length !== 2) { pinch = null; return; }
      const d = Math.hypot(e.touches[0].clientX - e.touches[1].clientX, e.touches[0].clientY - e.touches[1].clientY);
      if (pinch) this.zoomBy((pinch - d) * 0.004);
      pinch = d;
    }, { passive: true });
    dom.addEventListener('touchend', () => { pinch = null; });
    window.addEventListener('keydown', e => {
      if (e.key === '1') this.setView('walk');
      if (e.key === '2') this.setView('god');
      if (e.key === '3') this.setView('bird');
    });
  }

  zoomBy(d) {
    if (this.frozen) this.orbitFrozen.dist = Math.min(5, Math.max(0.8, this.orbitFrozen.dist + d * 3));
    else this.orbit.zoom = Math.min(1, Math.max(0.2, this.orbit.zoom + d));
  }

  pose(s) {
    if (this.frozen) {
      const f = this.frozen, o = this.orbitFrozen, a = f.angle + Math.PI + o.yaw;
      return { pos: [f.x + Math.cos(a) * o.dist * Math.cos(o.pitch), 0.6 + o.dist * Math.sin(o.pitch), f.y + Math.sin(a) * o.dist * Math.cos(o.pitch)],
               look: [f.x, 0.55, f.y], fov: 45 };
    }
    return viewPose(this.view, s, this.orbit);
  }

  // Advance springs toward the target pose; returns the walls to fade.
  update(s, dt, grid) {
    const p = this.pose(s), sp = this.springs;
    const keys = ['px', 'py', 'pz', 'lx', 'ly', 'lz'];
    const vals = [...p.pos, ...p.look];
    keys.forEach((k, i) => { sp[k].target = vals[i]; if (this.first) { sp[k].value = vals[i]; } });
    sp.fov.target = p.fov;
    if (this.first) sp.fov.value = p.fov;
    this.first = false;
    for (const k of Object.keys(sp)) sp[k].step(dt);
    this.camera.position.set(sp.px.value, sp.py.value, sp.pz.value);
    this.camera.lookAt(sp.lx.value, sp.ly.value, sp.lz.value);
    if (this.frozen) this.camera.rotateZ(0.06);             // bullet-time horizon tilt
    this.camera.fov = sp.fov.value;
    this.camera.updateProjectionMatrix();
    if (this.view === 'walk' && !this.frozen) return [];
    const target = this.frozen ? [this.frozen.x, this.frozen.y] : [s.x, s.y];
    return wallsBetween(grid, [this.camera.position.x, this.camera.position.z], target);
  }

  resize(w, h) { this.camera.aspect = w / h; this.camera.updateProjectionMatrix(); }
}

export { BLEND_S };
```

- [ ] **Step 2: Build check** — `npx esbuild src/cameras.js --bundle --format=esm --outfile=/tmp/cam-check.js`.

- [ ] **Step 3: Commit**

```bash
git add site/bwo-hq/src/cameras.js
git commit -m "BWO HQ: camera rigs, touch/mouse/keys input, bullet-time orbit"
```

---

### Task 11: Photo mode, HUD, and the drop-in renderer

**Files:** Create `site/bwo-hq/src/photo.js`, `site/bwo-hq/src/hud.js`, `site/bwo-hq/src/renderer.js`; modify `site/bwo-hq/src/main.js`

- [ ] **Step 1: `src/photo.js`**

```js
import * as THREE from 'three';
import { WebGLPathTracer, EquirectCamera } from 'three-gpu-pathtracer';

export const STILL_SAMPLES = 256;

function download(canvas, name) {
  canvas.toBlob(b => {
    if (!b) return;
    const a = document.createElement('a');
    a.href = URL.createObjectURL(b); a.download = name; a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 4000);
  }, 'image/png');
}

export class Photo {
  constructor(renderer) {
    this.renderer = renderer;
    this.job = null;
    try { this.tracer = new WebGLPathTracer(renderer); this.tracer.tiles.set(2, 2); }
    catch (e) { console.warn('path tracer unavailable, raster fallback', e); this.tracer = null; }
  }

  get busy() { return !!this.job; }

  // kind: 'still' | 'panorama'. onProgress(0..1). Resolves with { fallback }.
  start(kind, scene, camera, onProgress) {
    this.cancel();
    const stamp = new Date().toISOString().replace(/[:.]/g, '-');
    const name = `bwo-${kind}-${stamp}.png`;
    if (!this.tracer) {
      this.renderer.render(scene, camera);
      download(this.renderer.domElement, name);
      return Promise.resolve({ fallback: true });
    }
    let cam = camera;
    if (kind === 'panorama') {
      cam = new EquirectCamera();
      cam.position.copy(camera.position);
      this.prevSize = this.renderer.getSize(new THREE.Vector2());
      this.renderer.setSize(2048, 1024, false);
    }
    this.tracer.setScene(scene, cam);
    return new Promise((resolve, reject) => {
      this.job = { resolve, reject, kind, name };
      const step = () => {
        if (!this.job) return;
        try { this.tracer.renderSample(); }
        catch (e) { this.finish(); reject(e); return; }
        onProgress(Math.min(1, this.tracer.samples / STILL_SAMPLES));
        if (this.tracer.samples >= STILL_SAMPLES) {
          download(this.renderer.domElement, name);
          this.finish(); resolve({ fallback: false });
        } else requestAnimationFrame(step);
      };
      requestAnimationFrame(step);
    });
  }

  finish() {
    if (this.prevSize) { this.renderer.setSize(this.prevSize.x, this.prevSize.y, false); this.prevSize = null; }
    this.job = null;
  }

  cancel() { if (this.job) { const j = this.job; this.finish(); j.resolve({ cancelled: true }); } }
}
```

Check `WebGLPathTracer`'s API in `node_modules/three-gpu-pathtracer/README.md` for 0.0.24 (`setScene`, `renderSample`, `samples`, `tiles`); adjust names if they differ and report.

- [ ] **Step 2: `src/hud.js`**

```js
// Small overlay: view button, shutter, photo choice, progress ring, fps.
export class Hud {
  constructor(host, { onView, onShutter, onStill, onPanorama, onExit }) {
    this.el = document.createElement('div');
    this.el.className = 'bwo-hq-hud';
    this.el.innerHTML = `
      <style>
        .bwo-hq-hud{position:absolute;inset:0;pointer-events:none;font:12px/1.2 "Courier New",monospace;color:#e8f1ee}
        .bwo-hq-hud button{pointer-events:auto;background:#0b1416cc;color:inherit;border:1px solid #3e7777;padding:6px 10px;margin:4px;min-width:44px;min-height:44px}
        .bwo-hq-hud .tr{position:absolute;top:4px;right:4px}
        .bwo-hq-hud .photo{position:absolute;bottom:8px;left:50%;transform:translateX(-50%);display:none}
        .bwo-hq-hud .ring{position:absolute;top:50%;left:50%;width:56px;height:56px;margin:-28px;border-radius:50%;display:none}
        .bwo-hq-hud .fps{position:absolute;top:6px;left:8px;opacity:.7;display:none}
      </style>
      <div class="fps"></div>
      <div class="tr"><button data-a="view">WALK</button><button data-a="shutter" aria-label="shutter">&#9673;</button></div>
      <div class="photo"><button data-a="still">STILL</button><button data-a="pano">360</button><button data-a="exit">BACK</button></div>
      <div class="ring"></div>`;
    host.append(this.el);
    const act = { view: onView, shutter: onShutter, still: onStill, pano: onPanorama, exit: onExit };
    this.el.querySelectorAll('button').forEach(b => b.addEventListener('click', e => { e.stopPropagation(); act[b.dataset.a](); }));
    this.fpsEl = this.el.querySelector('.fps');
    if (new URLSearchParams(location.search).has('fps')) this.fpsEl.style.display = 'block';
  }
  setView(v) { this.el.querySelector('[data-a=view]').textContent = v.toUpperCase(); }
  photoMode(on) { this.el.querySelector('.photo').style.display = on ? 'block' : 'none'; }
  progress(p) {
    const r = this.el.querySelector('.ring');
    r.style.display = p > 0 && p < 1 ? 'block' : 'none';
    r.style.background = `conic-gradient(#f3c26a ${p * 360}deg, #0b141688 0)`;
  }
  fps(v) { this.fpsEl.textContent = `${v.toFixed(0)} fps`; }
}
```

- [ ] **Step 3: `src/renderer.js`**

```js
import * as THREE from 'three';
import { pickTier, Governor, TIERS } from './quality.js';
import { MaterialLibrary } from './materials.js';
import { World } from './world.js';
import { Lighting } from './lighting.js';
import { Post } from './post.js';
import { Actors } from './actors.js';
import { Cameras } from './cameras.js';
import { Photo } from './photo.js';
import { Hud } from './hud.js';

function gpuInfo(gl) {
  const ext = gl.getExtension('WEBGL_debug_renderer_info');
  return ext ? gl.getParameter(ext.UNMASKED_RENDERER_WEBGL) : '';
}

export class HQRenderer {
  constructor(host, art) {
    this.host = host;
    this.art = art;
    this.renderer = new THREE.WebGLRenderer({ antialias: false, powerPreference: 'high-performance', preserveDrawingBuffer: true });
    this.renderer.outputColorSpace = THREE.SRGBColorSpace;
    this.renderer.toneMapping = THREE.AgXToneMapping;
    this.renderer.shadowMap.enabled = true;
    this.renderer.shadowMap.type = THREE.PCFSoftShadowMap;
    this.renderer.domElement.id = 'rpg-scene';
    this.renderer.domElement.setAttribute('aria-label', 'BWO camera shrine world');
    host.style.position ||= 'relative';
    host.prepend(this.renderer.domElement);

    const params = new URLSearchParams(location.search);
    const mobile = /Android|iPhone|iPad|Mobile/i.test(navigator.userAgent);
    this.tierName = params.get('tier') || pickTier({ gpu: gpuInfo(this.renderer.getContext()), mobile, benchMs: 8 });
    this.tier = TIERS[this.tierName];
    this.governor = new Governor(this.tierName);

    this.scene = new THREE.Scene();
    this.mats = new MaterialLibrary();
    this.cams = new Cameras(this.renderer.domElement);
    this.lighting = new Lighting(this.renderer, this.scene, this.cams.camera, this.tier);
    this.world = new World(this.scene, this.mats);
    this.actors = new Actors(this.scene);
    this.post = new Post(this.renderer, this.scene, this.cams.camera, this.tier);
    this.photo = new Photo(this.renderer);
    this.hud = new Hud(host, {
      onView: () => this.hud.setView(this.toggleView()),
      onShutter: () => this.enterPhoto(),
      onStill: () => this.shoot('still'),
      onPanorama: () => this.shoot('panorama'),
      onExit: () => this.exitPhoto(),
    });
    this.lastT = 0; this.fpsAcc = 0; this.fpsN = 0;
    this.registered = new WeakSet();
    this.resize();
    new ResizeObserver(() => this.resize()).observe(host);
  }

  // ── drop-in contract ───────────────────────────────────────────────────
  get view() { return this.cams.view === 'walk' ? 'walk' : 'diorama'; }
  set view(v) { this.cams.setView(v === 'walk' ? 'walk' : v === 'diorama' ? 'god' : v); this.hud?.setView(this.cams.view); }
  setView(v) { const r = this.cams.setView(v); this.hud.setView(r); return r; }
  toggleView() { const r = this.cams.cycle(); return r === 'walk' ? 'walk' : 'diorama'; }
  syncPhotos(canvases) { this.mats.prunePhotos(canvases); return false; }
  rebuild(s) { this.world.forceRebuild(s); this.registerMaterials(); }

  registerMaterials() {
    this.scene.traverse(o => {
      const list = Array.isArray(o.material) ? o.material : o.material ? [o.material] : [];
      for (const m of list) if (!this.registered.has(m) && m.isMeshStandardMaterial) { this.lighting.setupMaterial(m); this.registered.add(m); }
    });
  }

  resize() {
    const w = this.host.clientWidth || 480, h = this.host.clientHeight || 252;
    const scale = this.governor.state.scale * Math.min(window.devicePixelRatio, 2);
    this.renderer.setPixelRatio(scale);
    this.renderer.setSize(w, h, false);
    this.renderer.domElement.style.width = '100%';
    this.renderer.domElement.style.height = '100%';
    this.post.setSize(w * scale, h * scale);
    this.cams.resize(w, h);
  }

  applyGovernor(action) {
    if (!action) return;
    if (action.kind === 'scale') this.resize();
    else if (action.what === 'cascades') this.lighting.setCascades(action.kind === 'on' ? this.tier.cascades : 1);
    else this.post.set(action.what === 'shafts' ? 'shafts' : action.what, action.kind === 'on');
  }

  enterPhoto() { if (this.state) { this.cams.freeze(this.state); this.hud.photoMode(true); } }
  exitPhoto() { this.photo.cancel(); this.cams.unfreeze(); this.hud.photoMode(false); this.hud.progress(0); }
  shoot(kind) {
    this.photo.start(kind, this.scene, this.cams.camera, p => this.hud.progress(p))
      .then(r => { this.hud.progress(0); if (r.fallback) console.info('raster capture (path tracer unavailable)'); })
      .catch(e => { this.hud.progress(0); console.warn('photo failed, retry available', e); });
  }

  render(s, t) {
    this.state = s;
    const dt = this.lastT ? Math.min(0.1, (t - this.lastT) / 1000) : 1 / 60;
    if (this.lastT) this.applyGovernor(this.governor.frame(t - this.lastT));
    this.lastT = t;
    if ((s.theme || 0) !== this.theme) { this.theme = s.theme || 0; this.lighting.setTheme(this.theme); }
    if (this.world.update(s)) this.registerMaterials();
    if (this.photo.busy) return;                          // path tracer owns the canvas
    const frozen = !!this.cams.frozen;
    const fade = this.cams.update(s, dt, s.world[s.level]);
    this.world.fade(fade);
    this.world.showCeilings(this.cams.view === 'walk' && !frozen);
    if (!frozen) this.actors.update(s, t, this.cams.view);
    else this.actors.player.visible = true;
    this.lighting.setCamera(this.cams.camera);
    this.lighting.update();
    this.post.setCamera(this.cams.camera);
    this.post.frame(t, frozen ? 0 : s.glitch || 0);
    this.fpsAcc += dt; this.fpsN++;
    if (this.fpsAcc > 0.5) { this.hud.fps(this.fpsN / this.fpsAcc); this.fpsAcc = 0; this.fpsN = 0; }
  }
}
```

- [ ] **Step 4: `src/main.js`** (replace the smoke content)

```js
import { HQRenderer } from './renderer.js';

window.WuwShrineRenderer = HQRenderer;
window.WuwRendererKind = 'hq';
window.dispatchEvent(new Event('wuw-renderer-ready'));
```

- [ ] **Step 5: Build and size check**

Run: `npm run build && ls -l hq.bundle.js && npm test`
Expected: build succeeds, tests pass. Report the bundle size.

- [ ] **Step 6: Commit**

```bash
git add site/bwo-hq/src site/bwo-hq/hq.bundle.js
git commit -m "BWO HQ: path-traced photo mode, HUD and drop-in renderer"
```

---

### Task 12: Load HQ from the game page with fallback

**Files:** Modify `site/game-preview/index.html` (the line that creates `rendererScript`, currently line 117)

- [ ] **Step 1: Replace the loader line**

Find:

```js
  const rendererScript=document.createElement('script');rendererScript.src=gameBase+'rpg.bundle.js';document.head.append(rendererScript);
```

Replace with:

```js
  const wantLite=new URLSearchParams(location.search).has('lite');
  const hasGl2=(()=>{try{return !!document.createElement('canvas').getContext('webgl2')}catch(e){return false}})();
  const rendererScript=document.createElement('script');
  rendererScript.src=(!wantLite&&hasGl2)?gameBase+'../bwo-hq/hq.bundle.js':gameBase+'rpg.bundle.js';
  rendererScript.onerror=()=>{const f=document.createElement('script');f.src=gameBase+'rpg.bundle.js';document.head.append(f)};
  if(!hasGl2&&!wantLite)console.info('WebGL2 unavailable: using the lite BWO renderer');
  document.head.append(rendererScript);
```

- [ ] **Step 2: Serve and smoke-test**

Run: `cd /Users/Browny/Downloads/esp32cam-bwo && python3 tools/preview_server.py` (check its `--help` for the port; it serves `site/`). Open the game page in a browser, enter BWO, confirm the HQ renderer draws, the console shows no errors, `?lite` loads the old renderer, `?fps` shows the fps readout, and `?tier=phone` / `?tier=desktop` switch tiers.

- [ ] **Step 3: Commit**

```bash
git add site/game-preview/index.html
git commit -m "Game page: load BWO HQ when WebGL2 is available, lite fallback"
```

---

### Task 13: Playwright checks and performance route

**Files:** Create `site/bwo-hq/playwright.config.js`, `site/bwo-hq/e2e/hq.spec.js`

- [ ] **Step 1: Install a browser** (owner approval: downloads Chromium for Playwright, ~150 MB)

Run: `cd site/bwo-hq && npx playwright install chromium`

- [ ] **Step 2: Config**

```js
// playwright.config.js
import { defineConfig, devices } from '@playwright/test';
export default defineConfig({
  testDir: 'e2e',
  webServer: { command: 'python3 -m http.server 8765 --directory ..', port: 8765, reuseExistingServer: true },
  use: { baseURL: 'http://localhost:8765/' },
  projects: [
    { name: 'desktop', use: { ...devices['Desktop Chrome'], viewport: { width: 1280, height: 800 } } },
    { name: 'phone', use: { ...devices['Pixel 7'], viewport: { width: 390, height: 844 } } },
  ],
});
```

- [ ] **Step 3: Tests** `e2e/hq.spec.js`. The game page needs a way into BWO: read `site/game-preview/index.html` for the hub entry (`[data-open="bwo"]`) and use it.

```js
import { test, expect } from '@playwright/test';

const PAGE = 'game-preview/index.html';

async function enterBwo(page) {
  const errors = [];
  page.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
  page.on('pageerror', e => errors.push(String(e)));
  await page.goto(PAGE + '?fps');
  await page.locator('[data-open="bwo"]').first().click();
  await page.waitForFunction(() => window.WuwRendererKind === 'hq');
  await page.waitForTimeout(1500);
  return errors;
}

test('HQ renderer loads with no console errors', async ({ page }) => {
  const errors = await enterBwo(page);
  await expect(page.locator('#rpg-scene')).toBeVisible();
  expect(errors).toEqual([]);
});

for (const view of ['walk', 'god', 'bird']) {
  test(`view ${view} renders`, async ({ page }) => {
    await enterBwo(page);
    const btn = page.locator('.bwo-hq-hud [data-a=view]');
    for (let i = 0; i < 3 && (await btn.textContent()).toLowerCase() !== view; i++) await btn.click();
    await page.waitForTimeout(800);   // longer than the 0.4 s blend
    await expect(page.locator('#rpg-scene')).toHaveScreenshot(`${view}.png`, { maxDiffPixelRatio: 0.03 });
  });
}

test('photo mode produces a PNG', async ({ page }) => {
  await enterBwo(page);
  await page.evaluate(() => { window.__lowSamples = true; });
  await page.locator('.bwo-hq-hud [data-a=shutter]').click();
  const download = page.waitForEvent('download', { timeout: 120000 });
  await page.locator('.bwo-hq-hud [data-a=still]').click();
  expect((await download).suggestedFilename()).toMatch(/^bwo-still-.*\.png$/);
});
```

Make `STILL_SAMPLES` honour the test hook: in `src/photo.js` change the constant to
`export const STILL_SAMPLES = () => (globalThis.__lowSamples ? 8 : 256);` and use `STILL_SAMPLES()` in both places. Rebuild.

- [ ] **Step 4: Run** — `npx playwright test --update-snapshots` once to create references, inspect the PNGs in `e2e/hq.spec.js-snapshots/` (send them to the owner for sign-off), then `npx playwright test`. Expected: all pass on both projects.

- [ ] **Step 5: Performance readout** — with `?fps&tier=desktop` on the Mac and `?fps&tier=phone` on a real phone, walk the route: start position, turn a full circle, walk to the far wall, switch through all three views. Record average fps in `docs/bwo-hq-perf.md`. Phone measurement is manual (owner).

- [ ] **Step 6: Commit**

```bash
git add site/bwo-hq/playwright.config.js site/bwo-hq/e2e site/bwo-hq/src/photo.js site/bwo-hq/hq.bundle.js
git commit -m "BWO HQ: Playwright view, console and photo-mode checks"
```

---

### Task 14: CC0 texture scans (owner approval for download)

**Files:** Create `site/bwo-hq/assets/` textures, `site/bwo-hq/assets/SOURCES.md`; modify `src/materials.js`

- [ ] **Step 1:** Ask the owner to approve downloading 1K JPG PBR sets (colour, normal, roughness; ~1-2 MB each) from ambientCG (CC0) for: weathered stone blocks, oxidised green metal, glazed red tiles, brushed metal panel, stone pavers, wood planks, plaster ceiling. List exact asset IDs and sizes in the request.
- [ ] **Step 2:** Download into `site/bwo-hq/assets/<asset-id>/`, record each in `SOURCES.md` (`asset id, URL, licence CC0, date`).
- [ ] **Step 3:** In `materials.js`, add `loadScan(id)` using `THREE.TextureLoader` with the procedural set as the initial value and the scan swapped in on load (streaming per spec); on error keep the procedural set.
- [ ] **Step 4:** Rebuild, rerun `npm test` and `npx playwright test --update-snapshots`, send new screenshots for sign-off.
- [ ] **Step 5:** Commit `git commit -m "BWO HQ: CC0 PBR scans with procedural fallback"`.

---

## Self-review notes

- Spec coverage: tiers + governor (T2), PBR materials + photo panels + placeholder (T5), bevelled world + incremental chunks (T3, T6), environment/CSM/lanterns (T7), GTAO/bloom/AA/tonemapping/glitch (T8), walk/god/bird + springs + wall fade + bullet-time + tilt (T4, T10), path-traced still/panorama + raster fallback + cancel + PNG save (T11), WebGL2 fallback (T12), peers with 1.5 s fade + placeholder figure (T9), tests + perf (T2-T4, T13), CC0 licensing (T14). Light shafts appear in the tier table and degrade order but have no pass yet: the governor toggles a `shafts` flag that `Post.set` ignores until a shafts pass exists; adding a shafts pass is deferred to a follow-up plan to keep this one shippable.
- Gallery upload to the camera is out of scope until a firmware endpoint exists (spec: local save only for now).
- Names are consistent across tasks: `planChunk`, `changedChunks`, `snapshot`, `Spring`, `viewPose`, `wallsBetween`, `Governor.frame`, `World.update/forceRebuild/fade/showCeilings`, `Cameras.update/freeze/unfreeze/cycle/setView`, `Photo.start/cancel/busy`.
