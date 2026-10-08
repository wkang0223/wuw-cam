// Node test for the browser preview's pilgrim-name rules; must agree with tools/bwo_name_test.cpp.
//   node tools/test_pilgrim_name.cjs
const assert = require('assert');
const N = require('../site/game-preview/pilgrim-name.js');
const cases = [
  ['mika', 'MIKA'], ['W8I', 'W8I'], ['  moon  rabbit  ', 'MOON RABBIT'], ['a-b_c.d', 'A-B_C.D'],
  ['ZERO<script>', 'ZEROSCRIPT'], ['naïve', 'NAVE'], ['ABCDEFGHIJKLMNOP', 'ABCDEFGHIJKL'],
  ['ABCDEFGHIJK LMN', 'ABCDEFGHIJK'], ['', 'Q'], ['   ', 'Q'], ['%%%', 'Q'], [null, 'Q'], [undefined, 'Q'],
];
let failed = 0;
for (const [input, want] of cases) {
  const got = N.clean(input);
  const ok = got === want;
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${JSON.stringify(input)} -> ${JSON.stringify(got)}`);
  if (!ok) failed++;
}
const store = new Map();
const storage = { getItem: k => (store.has(k) ? store.get(k) : null), setItem: (k, v) => store.set(k, v), removeItem: k => store.delete(k) };
assert.strictEqual(N.load(storage), 'Q');
assert.strictEqual(N.save(storage, 'zero one'), 'ZERO ONE');
assert.strictEqual(N.load(storage), 'ZERO ONE');
assert.strictEqual(N.save(storage, ''), 'Q');
assert.ok(!store.has(N.KEY), 'default is stored as no key');
assert.strictEqual(N.load({ getItem() { throw new Error('blocked'); } }), 'Q');
console.log(failed ? `${failed} failure(s)` : 'all pass');
process.exit(failed ? 1 : 0);
