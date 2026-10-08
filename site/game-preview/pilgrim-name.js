/* The pilgrim's name: what the player calls their BWO character. The same rules as the camera
   firmware (bwo_name.h): A-Z 0-9 space - _ . only, upper case, trimmed, no doubled spaces, at most 12
   characters, and "W8I" when nothing is left. */
(function (root) {
  'use strict';
  const MAX = 12, DEFAULT = 'Q', KEY = 'wuwBwoName';
  function clean(input) {
    let out = '';
    for (const ch of String(input == null ? '' : input).toUpperCase()) {
      if (out.length >= MAX) break;
      if (!/^[A-Z0-9 _.-]$/.test(ch)) continue;
      if (ch === ' ' && (out === '' || out.endsWith(' '))) continue;
      out += ch;
    }
    out = out.replace(/ +$/, '');
    return out || DEFAULT;
  }
  function load(storage) {
    try { return clean(storage.getItem(KEY)); } catch (e) { return DEFAULT; }
  }
  function save(storage, name) {
    const value = clean(name);
    try { if (value === DEFAULT) storage.removeItem(KEY); else storage.setItem(KEY, value); } catch (e) { /* private mode: the name lasts until reload */ }
    return value;
  }
  const api = { clean, load, save, MAX, DEFAULT, KEY };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.WuwPilgrimName = api;
})(typeof window !== 'undefined' ? window : globalThis);
