import test from 'node:test';
import assert from 'node:assert/strict';
import { planChunk, chunkOf, changedChunks, snapshot, CHUNK, SIZE } from '../src/world-plan.js';

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

test('no previous state dirties everything', () => {
  assert.equal(changedChunks(null, emptyState()).size, 16);
});

test('identical states change nothing', () => {
  assert.equal(changedChunks(emptyState(), emptyState()).size, 0);
});

test('snapshot is independent of later edits to the original', () => {
  const s = emptyState();
  s.world[0][4][4] = 3;
  s.materials[0][4][4] = [1, 2, 3, 4, 5, 6];
  const snap = snapshot(s);
  s.world[0][4][4] = 0;
  s.materials[0][4][4][0] = 99;
  assert.equal(snap.world[0][4][4], 3);
  assert.equal(snap.materials[0][4][4][0], 1);
  assert.equal(changedChunks(snap, s).size, 1);
  assert.equal(changedChunks(snapshot(s), s).size, 0);
});
