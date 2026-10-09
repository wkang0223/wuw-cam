// Procedural PBR material library for the BWO HQ renderer.
//
// Every surface is generated on 256x256 canvases (albedo, tangent-space normal,
// roughness) from a single per-pixel surface function, so the normal map is
// derived from the very same height field that defines mortar, seams and grout.
// Textures tile seamlessly and are deterministic from the seed. Real CC0 scans
// replace these in a later task. Browser only (uses document/canvas).
import * as THREE from 'three';

const N = 256;

// Theme base colours (albedo) for the four wall regions, floor, ceiling.
const THEMES = [
  { walls: [0xd8d0bf, 0x5f8f7c, 0x8e2f2a, 0x9a9da0], floor: 0x6f776b, ceil: 0xe2dccb },
  { walls: [0xb8b3a6, 0x3f6b5e, 0x6e2423, 0x7d8285], floor: 0x575d55, ceil: 0xc9c3b3 },
  { walls: [0x9e8f86, 0x4c5f57, 0x7a1f26, 0x6a6466], floor: 0x4a3a33, ceil: 0xb5a99c },
  { walls: [0xe6e1d3, 0x7fa898, 0xa4473d, 0xb7babc], floor: 0x7d857b, ceil: 0xf0ebdc },
];
const ROUGH = [0.82, 0.38, 0.45, 0.55];   // stone, jade metal, glazed ceramic, machinery
const METAL = [0.0, 0.6, 0.0, 0.75];
const WALL_KINDS = ['stone', 'metal', 'ceramic', 'machinery'];
const FLOOR_ROUGH = 0.72;
const CEIL_ROUGH = 0.9;

// ---------------------------------------------------------------- noise ----

// Integer hash -> [0,1).
function hash2(x, y, s) {
  let h = Math.imul(x | 0, 374761393) ^ Math.imul(y | 0, 668265263) ^ Math.imul(s | 0, 1442695041);
  h = Math.imul(h ^ (h >>> 13), 1274126177);
  h ^= h >>> 16;
  return (h >>> 0) / 4294967296;
}

const smooth = (a, b, x) => {
  const t = Math.min(1, Math.max(0, (x - a) / (b - a)));
  return t * t * (3 - 2 * t);
};
const lerp = (a, b, t) => a + (b - a) * t;

// Seeded value noise that tiles over u,v in [0,1) with px x py lattice cells.
function vnoise(u, v, px, py, s) {
  const x = u * px, y = v * py;
  const ix = Math.floor(x), iy = Math.floor(y);
  let fx = x - ix, fy = y - iy;
  fx = fx * fx * (3 - 2 * fx);
  fy = fy * fy * (3 - 2 * fy);
  const x0 = ((ix % px) + px) % px, x1 = (x0 + 1) % px;
  const y0 = ((iy % py) + py) % py, y1 = (y0 + 1) % py;
  const a = hash2(x0, y0, s), b = hash2(x1, y0, s);
  const c = hash2(x0, y1, s), d = hash2(x1, y1, s);
  return lerp(lerp(a, b, fx), lerp(c, d, fx), fy);
}

// Fractal sum of tiling value noise, normalised to roughly [0,1].
function fbm(u, v, px, py, s, oct) {
  let sum = 0, amp = 1, norm = 0;
  for (let o = 0; o < oct; o++) {
    sum += amp * vnoise(u, v, px << o, py << o, s + o * 101);
    norm += amp;
    amp *= 0.5;
  }
  return sum / norm;
}

// Small seeded LCG for layout decisions.
function lcg(seed) {
  let r = seed >>> 0;
  return () => ((r = (Math.imul(r, 1664525) + 1013904223) >>> 0) / 4294967296);
}

const rgb = (hex) => [(hex >> 16) & 255, (hex >> 8) & 255, hex & 255];
const scale = (c, k) => [c[0] * k, c[1] * k, c[2] * k];

// -------------------------------------------------------------- surfaces ---
// Each factory returns { edge, px } where px(x, y, o) fills the shared output:
//   o.h      height 0..~1 (drives the normal map)
//   o.m      albedo brightness multiplier
//   o.tr/tg/tb additive albedo tint (0..255 units)
//   o.mortar 0..1 blend toward the `edge` colour (mortar, grout, seams)
//   o.rough  roughness 0..1

const SURFACES = {
  // Staggered ashlar blocks, half-block offset on odd rows, mortar and grain.
  stone(seed, rough, base) {
    const BW = 64, BH = 32;
    return {
      edge: scale(base, 0.5), k: 5,
      px(x, y, o) {
        const row = (y / BH) | 0;
        const xs = x + ((row & 1) ? BW / 2 : 0);
        const col = Math.floor(xs / BW);
        const bx = xs - col * BW, by = y - row * BH;
        const d = Math.min(bx, BW - bx, by, BH - by) + 0.5;
        const id = hash2(col & 3, row, seed);
        const u = x / N, v = y / N;
        const grain = fbm(u, v, 12, 12, seed + 1, 3);
        const fine = vnoise(u, v, 64, 64, seed + 2);
        const face = smooth(0.8, 3.6, d);
        o.h = face * 0.55 + grain * 0.3 + fine * 0.08 + (id - 0.5) * 0.1 * face;
        o.m = 0.8 + id * 0.3 + (grain - 0.5) * 0.28 + (fine - 0.5) * 0.12;
        o.mortar = 1 - smooth(0.8, 2.4, d);
        o.rough = lerp(rough + (grain - 0.5) * 0.14 + (fine - 0.5) * 0.06, 0.96, o.mortar);
        o.tr = o.tg = o.tb = 0;
      },
    };
  },

  // Brushed oxidised metal: horizontal streaks plus pale patina blotches.
  metal(seed, rough, base) {
    return {
      edge: scale(base, 0.6), k: 8,
      px(x, y, o) {
        const u = x / N, v = y / N;
        const streak = vnoise(u, v, 2, 128, seed);
        const fine = vnoise(u, v, 4, 256, seed + 1);
        const blotch = fbm(u, v, 5, 5, seed + 3, 4);
        const p = smooth(0.5, 0.68, blotch);
        o.h = (streak - 0.5) * 0.09 + (fine - 0.5) * 0.05 + p * (0.12 + blotch * 0.12);
        o.m = 0.82 + streak * 0.26 + (fine - 0.5) * 0.1 - p * 0.05;
        o.tr = 30 * p; o.tg = 36 * p; o.tb = 26 * p;
        o.mortar = 0;
        o.rough = rough + (streak - 0.5) * 0.28 + (fine - 0.5) * 0.1 + p * 0.35;
      },
    };
  },

  // Square glazed tiles: glossy faces, matte recessed grout.
  ceramic(seed, rough, base) {
    const T = 32;
    return {
      edge: [base[0] * 0.35 + 150, base[1] * 0.35 + 146, base[2] * 0.35 + 138], k: 4,
      px(x, y, o) {
        const tx = (x / T) | 0, ty = (y / T) | 0;
        const bx = x - tx * T, by = y - ty * T;
        const d = Math.min(bx, T - bx, by, T - by) + 0.5;
        const id = hash2(tx, ty, seed);
        const u = x / N, v = y / N;
        const gloss = fbm(u, v, 8, 8, seed + 1, 2);
        const face = smooth(1.2, 3.8, d);
        o.h = face * 0.5 + (gloss - 0.5) * 0.05 + (id - 0.5) * 0.03 * face;
        o.m = 0.9 + id * 0.16 + (gloss - 0.5) * 0.1;
        o.mortar = 1 - smooth(0.9, 2.1, d);
        // glazed face is low roughness, grout is high
        o.rough = lerp(rough * 0.45 + gloss * 0.07 + id * 0.04, 0.92, o.mortar);
        o.tr = o.tg = o.tb = 0;
      },
    };
  },

  // Riveted machinery plating: 128x64 panels, seam grooves, rivet domes.
  machinery(seed, rough, base) {
    const PW = 128, PH = 64;
    const rxs = [8, 64, 120], rys = [8, 56];
    return {
      edge: scale(base, 0.38), k: 5,
      px(x, y, o) {
        const pxn = (x / PW) | 0, pyn = (y / PH) | 0;
        const bx = x - pxn * PW, by = y - pyn * PH;
        const d = Math.min(bx, PW - bx, by, PH - by) + 0.5;
        const id = hash2(pxn, pyn, seed);
        const u = x / N, v = y / N;
        const brushed = vnoise(u, v, 2, 192, seed + 1);
        const wear = fbm(u, v, 6, 6, seed + 2, 3);
        let dx = 1e9, dy = 1e9;
        for (const r of rxs) dx = Math.min(dx, Math.abs(bx - r));
        for (const r of rys) dy = Math.min(dy, Math.abs(by - r));
        const rd = Math.hypot(dx, dy);
        const dome = rd < 3.2 ? Math.sqrt(1 - (rd / 3.2) * (rd / 3.2)) : 0;
        const ring = smooth(3.0, 3.6, rd) * (1 - smooth(4.2, 5.4, rd));
        const face = smooth(0.8, 3.2, d);
        o.h = face * 0.5 + dome * 0.32 + (brushed - 0.5) * 0.05;
        o.m = 0.84 + id * 0.2 + (wear - 0.5) * 0.2 + (brushed - 0.5) * 0.08
          + dome * 0.12 - ring * 0.18;
        o.mortar = 1 - smooth(0.8, 2.2, d);
        o.rough = lerp(rough + (brushed - 0.5) * 0.16 + (wear - 0.5) * 0.1 + (id - 0.5) * 0.08, 0.86, o.mortar);
        o.rough = lerp(o.rough, rough * 0.65, dome);
        o.tr = o.tg = o.tb = 0;
      },
    };
  },

  // Stone paving: rows of varied height, slabs of varied width, dark joints.
  pavers(seed, rough, base) {
    const rnd = lcg(seed * 7 + 3);
    const split = (total, lo, span, minLast) => {
      const parts = [];
      let rem = total;
      while (rem > 0) {
        let s = lo + Math.floor(rnd() * span);
        if (rem - s < minLast) s = rem;
        parts.push(s);
        rem -= s;
      }
      return parts;
    };
    const rowH = split(N, 52, 48, 44);
    const rowOf = new Uint8Array(N);
    const rows = [];
    let y0 = 0;
    for (const h of rowH) {
      const widths = split(N, 56, 64, 48);
      const slabAt = new Uint8Array(N);
      const cells = [];
      let x0 = 0;
      widths.forEach((w, i) => {
        slabAt.fill(i, x0, x0 + w);
        cells.push({ x0, x1: x0 + w - 1, id: rnd(), lift: rnd() });
        x0 += w;
      });
      rows.push({ y0, y1: y0 + h - 1, slabAt, cells });
      rowOf.fill(rows.length - 1, y0, y0 + h);
      y0 += h;
    }
    return {
      edge: scale(base, 0.42), k: 5,
      px(x, y, o) {
        const row = rows[rowOf[y]];
        const c = row.cells[row.slabAt[x]];
        const d = Math.min(x - c.x0, c.x1 - x, y - row.y0, row.y1 - y) + 0.5;
        const u = x / N, v = y / N;
        const wear = fbm(u, v, 10, 10, seed + 1, 4);
        const fine = vnoise(u, v, 96, 96, seed + 2);
        const face = smooth(0.8, 3.4, d);
        o.h = face * 0.5 + c.lift * 0.12 * face + wear * 0.25 + fine * 0.06;
        o.m = 0.78 + c.id * 0.36 + (wear - 0.5) * 0.3 + (fine - 0.5) * 0.1;
        o.mortar = 1 - smooth(0.8, 2.6, d);
        o.rough = lerp(rough + (wear - 0.5) * 0.24 + (fine - 0.5) * 0.06, 0.97, o.mortar);
        o.tr = o.tg = o.tb = 0;
      },
    };
  },

  // Coffered plaster ceiling: 128px cells, raised beams, stepped recesses.
  coffer(seed, rough, base) {
    return {
      edge: scale(base, 0.8), k: 4,
      px(x, y, o) {
        const cx = x & 127, cy = y & 127;
        const d = Math.min(cx, 128 - cx, cy, 128 - cy);
        const r = Math.hypot(cx - 64, cy - 64);
        const u = x / N, v = y / N;
        const grain = fbm(u, v, 20, 20, seed + 1, 3);
        const fine = vnoise(u, v, 128, 128, seed + 2);
        const recess = smooth(14, 20, d);
        const inner = smooth(34, 40, d);
        const rosette = 1 - smooth(8, 12, r);
        const ring = smooth(17, 19, r) * (1 - smooth(21, 23, r));
        o.h = 1 - 0.7 * recess - 0.08 * inner + rosette * 0.25 + ring * 0.1
          + (grain - 0.5) * 0.05 + (fine - 0.5) * 0.02;
        o.m = 1.0 - recess * 0.1 - inner * 0.03 + (grain - 0.5) * 0.12 + (fine - 0.5) * 0.06;
        o.mortar = 0;
        o.rough = rough + (grain - 0.5) * 0.1 + (fine - 0.5) * 0.04;
        o.tr = o.tg = o.tb = 0;
      },
    };
  },
};

// ------------------------------------------------------------- texture set -

function canvasFrom(size, data) {
  const c = document.createElement('canvas');
  c.width = c.height = size;
  c.getContext('2d').putImageData(new ImageData(data, size, size), 0, 0);
  return c;
}

function tex(canvas, srgb) {
  const t = new THREE.CanvasTexture(canvas);
  t.wrapS = t.wrapT = THREE.RepeatWrapping;
  t.anisotropy = 8;
  if (srgb) t.colorSpace = THREE.SRGBColorSpace;
  return t;
}

// Builds { map, normalMap, roughnessMap } for one surface kind.
function pbrSet(hex, seed, rough, kind) {
  const base = rgb(hex);
  const surf = (SURFACES[kind] || SURFACES.stone)(seed, rough, base);
  const o = { h: 0, m: 1, tr: 0, tg: 0, tb: 0, mortar: 0, rough: rough };
  const height = new Float32Array(N * N);
  const alb = new Uint8ClampedArray(N * N * 4);
  const rgh = new Uint8ClampedArray(N * N * 4);
  const [er, eg, eb] = surf.edge;

  for (let y = 0; y < N; y++) {
    for (let x = 0; x < N; x++) {
      const i = y * N + x;
      surf.px(x, y, o);
      height[i] = o.h;
      const t = o.mortar;
      alb[i * 4] = lerp(base[0] * o.m + o.tr, er, t);
      alb[i * 4 + 1] = lerp(base[1] * o.m + o.tg, eg, t);
      alb[i * 4 + 2] = lerp(base[2] * o.m + o.tb, eb, t);
      alb[i * 4 + 3] = 255;
      const rv = Math.min(1, Math.max(0.04, o.rough)) * 255;
      rgh[i * 4] = rgh[i * 4 + 1] = rgh[i * 4 + 2] = rv;
      rgh[i * 4 + 3] = 255;
    }
  }

  // Tangent-space normals (OpenGL, +Z out of the surface) from central
  // differences of the same height field, wrapping at the edges so the
  // texture tiles. Canvas y runs down while texture v runs up, hence the
  // sign on ny.
  const nrm = new Uint8ClampedArray(N * N * 4);
  const k = surf.k;
  for (let y = 0; y < N; y++) {
    const ym = ((y - 1 + N) % N) * N, y0 = y * N, yp = ((y + 1) % N) * N;
    for (let x = 0; x < N; x++) {
      const xm = (x - 1 + N) % N, xp = (x + 1) % N;
      const dhdx = (height[y0 + xp] - height[y0 + xm]) * 0.5;
      const dhdy = (height[yp + x] - height[ym + x]) * 0.5;
      let nx = -dhdx * k, ny = dhdy * k, nz = 1;
      const len = Math.hypot(nx, ny, nz);
      nx /= len; ny /= len; nz /= len;
      const i = (y0 + x) * 4;
      nrm[i] = (nx * 0.5 + 0.5) * 255;
      nrm[i + 1] = (ny * 0.5 + 0.5) * 255;
      nrm[i + 2] = (nz * 0.5 + 0.5) * 255;
      nrm[i + 3] = 255;
    }
  }

  return {
    map: tex(canvasFrom(N, alb), true),
    normalMap: tex(canvasFrom(N, nrm), false),
    roughnessMap: tex(canvasFrom(N, rgh), false),
  };
}

// ---------------------------------------------------------- photo panels ---

function photoTexture(canvas) {
  const t = new THREE.CanvasTexture(canvas);
  t.colorSpace = THREE.SRGBColorSpace;
  t.generateMipmaps = true;
  t.minFilter = THREE.LinearMipmapLinearFilter;
  t.magFilter = THREE.LinearFilter;
  t.anisotropy = 8;
  t.needsUpdate = true;
  return t;
}

function photoMat(canvas) {
  const map = photoTexture(canvas);
  return new THREE.MeshStandardMaterial({
    map, emissiveMap: map, emissive: 0xffffff, emissiveIntensity: 0.35, roughness: 0.4,
  });
}

function placeholderCanvas() {
  const c = document.createElement('canvas');
  c.width = c.height = 64;
  const ctx = c.getContext('2d');
  ctx.fillStyle = 'rgb(180,160,120)';
  ctx.fillRect(0, 0, 64, 64);
  ctx.fillStyle = 'rgb(40,44,46)';
  ctx.fillRect(4, 4, 56, 56);
  return c;
}

// ---------------------------------------------------------------- library --

const clampIdx = (v) => {
  const n = Math.floor(Number(v));
  return Number.isFinite(n) ? Math.min(3, Math.max(0, n)) : 0;
};

export class MaterialLibrary {
  constructor() {
    this._walls = new Map();
    this._floors = new Map();
    this._photos = new Map();
    this._placeholder = null;
  }

  // Wall material for theme 0..3, region 0..3 (stone, jade metal, glazed
  // ceramic, machinery). Out-of-range input is clamped.
  wall(theme, region) {
    const t = clampIdx(theme), r = clampIdx(region);
    const key = t * 4 + r;
    let m = this._walls.get(key);
    if (!m) {
      const set = pbrSet(THEMES[t].walls[r], 1000 + t * 131 + r * 17, ROUGH[r], WALL_KINDS[r]);
      m = new THREE.MeshStandardMaterial({ ...set, roughness: 1, metalness: METAL[r] });
      this._walls.set(key, m);
    }
    return m;
  }

  // Stone paver floor, or coffered plaster ceiling when `ceiling` is true.
  floor(theme, ceiling) {
    const t = clampIdx(theme);
    const ceil = !!ceiling;
    const key = t * 2 + (ceil ? 1 : 0);
    let m = this._floors.get(key);
    if (!m) {
      const set = ceil
        ? pbrSet(THEMES[t].ceil, 3000 + t * 131, CEIL_ROUGH, 'coffer')
        : pbrSet(THEMES[t].floor, 2000 + t * 131, FLOOR_ROUGH, 'pavers');
      m = new THREE.MeshStandardMaterial({ ...set, roughness: 1, metalness: 0 });
      this._floors.set(key, m);
    }
    return m;
  }

  // Softly self-lit panel for a player photo canvas; missing photo -> placeholder.
  photoMaterial(canvas) {
    if (!canvas) return this.placeholder();
    let m = this._photos.get(canvas);
    if (!m) {
      m = photoMat(canvas);
      this._photos.set(canvas, m);
    }
    return m;
  }

  // Framed dark placeholder shown when a player has no photo.
  placeholder() {
    if (!this._placeholder) this._placeholder = photoMat(placeholderCanvas());
    return this._placeholder;
  }

  // Dispose and forget photo materials whose canvas is not in `canvases`.
  prunePhotos(canvases) {
    const keep = new Set(canvases || []);
    for (const [canvas, m] of this._photos) {
      if (keep.has(canvas)) continue;
      if (m.map) m.map.dispose();
      m.dispose();
      this._photos.delete(canvas);
    }
  }
}
