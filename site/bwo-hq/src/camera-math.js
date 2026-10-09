// Camera poses, springs and line-of-sight for wall fading. Pure.

export const VIEWS = ['walk', 'god', 'bird'];
export const BLEND_S = 0.4;

// Critically damped spring: ~90% of the way to target after `settle` seconds.
// Integrates the exact solution x(t) = (x0 + (v0 + w x0) t) e^(-wt), so it is
// stable for any dt.
export class Spring {
  constructor(value, settle = BLEND_S) {
    this.value = value; this.target = value; this.vel = 0;
    this.omega = 3.9 / settle;   // (1 + wt) e^-wt ~= 0.1 at wt ~= 3.89
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
// The camera's own cell and the player's cell are excluded; cells outside the
// grid are skipped, so the camera may sit outside it.
export function wallsBetween(grid, [x0, z0], [x1, z1]) {
  const out = [];
  let cx = Math.floor(x0), cz = Math.floor(z0);
  const ex = Math.floor(x1), ez = Math.floor(z1);
  const dx = x1 - x0, dz = z1 - z0;
  const sx = Math.sign(dx), sz = Math.sign(dz);
  const tdx = dx ? Math.abs(1 / dx) : Infinity, tdz = dz ? Math.abs(1 / dz) : Infinity;
  let tx = dx ? ((sx > 0 ? cx + 1 - x0 : x0 - cx) * tdx) : Infinity;
  let tz = dz ? ((sz > 0 ? cz + 1 - z0 : z0 - cz) * tdz) : Infinity;
  // Every DDA step moves one cell towards the end cell, so the Manhattan
  // distance bounds the walk; the +2 is slack for float edge cases.
  const maxSteps = Math.abs(ex - cx) + Math.abs(ez - cz) + 2;
  const rows = grid.length, cols = rows ? grid[0].length : 0;
  for (let i = 0; i < maxSteps && !(cx === ex && cz === ez); i++) {
    if (tx < tz) { cx += sx; tx += tdx; } else { cz += sz; tz += tdz; }
    if (cx === ex && cz === ez) break;
    if (cz >= 0 && cx >= 0 && cz < rows && cx < cols && grid[cz][cx]) out.push([cx, cz]);
  }
  return out;
}
