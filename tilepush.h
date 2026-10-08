/* ── Tile-run walker ──────────────────────────────────────────────────────
 * The one piece of the changed-tile push that is pure logic: given a grid of
 * tiles and a way to ask whether one changed, emit the horizontal RUNS of
 * changed tiles, so a moving subject costs a few wide rectangles instead of
 * dozens of small ones.
 *
 * It lives in its own header, free of Arduino and LovyanGFX, for one reason:
 * the panel it serves does not exist yet. Boundary handling on a tile grid --
 * the partial tile at the right edge, the run that is still open when the row
 * ends -- is exactly the kind of code that looks obviously correct and is off
 * by one, and waiting for hardware to find that out is a bad trade when the
 * logic can be run on a laptop today.
 *
 * tools/tiletest.cpp compiles THIS header, not a copy of it.
 */
#pragma once
#include <stddef.h>

/* changed(tx, ty)          -> bool, did this tile change
 * emit(x0, y0, wPx, hPx)   -> push this rectangle, in pixels
 *
 * w, h are the image size in pixels; tiles are tileSz square. The rightmost
 * and bottom tiles are clipped to the image, so emitted rectangles never
 * describe a pixel outside it. */
template <typename ChangedFn, typename EmitFn>
inline int tileRuns(int w, int h, int tileSz, ChangedFn changed, EmitFn emit) {
  if (w <= 0 || h <= 0 || tileSz <= 0) return 0;
  const int nx = (w + tileSz - 1) / tileSz;
  const int ny = (h + tileSz - 1) / tileSz;
  int sent = 0;
  for (int ty = 0; ty < ny; ty++) {
    const int y0 = ty * tileSz;
    const int th = (y0 + tileSz > h) ? (h - y0) : tileSz;
    int run = -1;
    /* One past the last column, so a run still open at the right edge is
       closed by the same branch that closes every other run. */
    for (int tx = 0; tx <= nx; tx++) {
      const bool inside = (tx < nx);
      const bool dirty  = inside && changed(tx, ty);
      if (dirty && run < 0) run = tx;
      if (!dirty && run >= 0) {
        const int x0 = run * tileSz;
        int rw = tx * tileSz - x0;
        if (x0 + rw > w) rw = w - x0;          // clip the partial edge tile
        emit(x0, y0, rw, th);
        sent += tx - run;
        run = -1;
      }
    }
  }
  return sent;
}
