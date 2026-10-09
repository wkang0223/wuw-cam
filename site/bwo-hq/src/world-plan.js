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
