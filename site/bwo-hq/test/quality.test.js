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
