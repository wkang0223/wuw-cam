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
