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

test('spring is stable with a large time step', () => {
  const s = new Spring(0, BLEND_S);
  s.target = 1;
  for (let i = 0; i < 10; i++) s.step(0.1);
  assert.ok(Math.abs(s.value - 1) < 0.05 && Number.isFinite(s.value));
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
  assert.ok(p.pos[0] < st.x, 'behind the player (player faces +x)');
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

test('wallsBetween works diagonally', () => {
  const grid = Array.from({ length: 24 }, () => Array(24).fill(0));
  grid[4][4] = 1;
  const hit = wallsBetween(grid, [2.5, 2.5], [6.5, 6.5]);
  assert.deepEqual(hit.map(([x, z]) => `${x},${z}`), ['4,4']);
});

test('wallsBetween ignores the player and camera cells', () => {
  const grid = Array.from({ length: 24 }, () => Array(24).fill(1));
  assert.equal(wallsBetween(grid, [3.5, 3.5], [3.6, 3.6]).length, 0);
});

test('wallsBetween tolerates a camera outside the grid', () => {
  const grid = Array.from({ length: 24 }, () => Array(24).fill(0));
  grid[5][2] = 1;
  const hit = wallsBetween(grid, [-3.5, 5.5], [6.5, 5.5]);
  assert.deepEqual(hit.map(([x, z]) => `${x},${z}`), ['2,5']);
});
