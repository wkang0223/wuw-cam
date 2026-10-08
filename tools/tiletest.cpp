/* Exercises tilepush.h -- the SAME header panel.cpp compiles, not a copy.
 *
 * The panel this serves does not exist yet, so these invariants are the only
 * thing standing between a boundary bug and a first bring-up spent chasing it:
 *
 *   1. every changed pixel is covered by exactly one emitted rectangle
 *   2. no unchanged tile is ever pushed
 *   3. no rectangle describes a pixel outside the image
 *   4. adjacent changed tiles merge into one rectangle, which is the point
 *   5. it holds for image sizes that are not a whole number of tiles
 *
 *   c++ -std=c++17 -O1 -Wall -Wextra -o tiletest tools/tiletest.cpp && ./tiletest
 */
#include "../tilepush.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <random>

struct Rect { int x, y, w, h; };
static int failures = 0;

static void check(bool ok, const char* what) {
  if (!ok) { printf("  FAIL  %s\n", what); failures++; }
}

/* Runs the walker over a pixel-level "changed" map and verifies coverage. */
static void run(const char* name, int w, int h, int tile,
                const std::vector<bool>& dirtyTile, int nx, int ny,
                bool expectMerge = false) {
  std::vector<Rect> rects;
  int sent = tileRuns(w, h, tile,
    [&](int tx, int ty) { return dirtyTile[(size_t)ty * nx + tx]; },
    [&](int x, int y, int rw, int rh) { rects.push_back({x, y, rw, rh}); });

  printf("%-34s %2d tiles in %2zu rect(s)\n", name, sent, rects.size());

  /* 3. bounds */
  for (auto& r : rects) {
    check(r.x >= 0 && r.y >= 0, "rect origin negative");
    check(r.x + r.w <= w, "rect runs past the right edge");
    check(r.y + r.h <= h, "rect runs past the bottom edge");
    check(r.w > 0 && r.h > 0, "empty rect emitted");
  }

  /* 1 + 2. coverage, counted per pixel */
  std::vector<int> cover((size_t)w * h, 0);
  for (auto& r : rects)
    for (int y = r.y; y < r.y + r.h; y++)
      for (int x = r.x; x < r.x + r.w; x++)
        cover[(size_t)y * w + x]++;

  for (int ty = 0; ty < ny; ty++)
    for (int tx = 0; tx < nx; tx++) {
      bool d = dirtyTile[(size_t)ty * nx + tx];
      int x0 = tx * tile, y0 = ty * tile;
      int x1 = (x0 + tile > w) ? w : x0 + tile;
      int y1 = (y0 + tile > h) ? h : y0 + tile;
      for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
          int c = cover[(size_t)y * w + x];
          if (d)  check(c == 1, "changed pixel not covered exactly once");
          else    check(c == 0, "unchanged pixel was pushed");
        }
    }

  /* 4. merging */
  if (expectMerge) check(rects.size() == 1, "adjacent tiles did not merge");
}

int main() {
  const int W = 320, H = 240, T = 16;
  const int NX = (W + T - 1) / T, NY = (H + T - 1) / T;

  { std::vector<bool> d((size_t)NX * NY, false);
    run("nothing changed", W, H, T, d, NX, NY);
    check(true, ""); }

  { std::vector<bool> d((size_t)NX * NY, true);
    run("everything changed", W, H, T, d, NX, NY); }

  { std::vector<bool> d((size_t)NX * NY, false);
    d[5 * NX + 7] = true;
    run("one tile", W, H, T, d, NX, NY, true); }

  { std::vector<bool> d((size_t)NX * NY, false);
    for (int i = 3; i <= 9; i++) d[4 * NX + i] = true;
    run("seven adjacent -> one rect", W, H, T, d, NX, NY, true); }

  { std::vector<bool> d((size_t)NX * NY, false);
    d[2 * NX + (NX - 1)] = true;              // hard against the right edge
    run("rightmost column", W, H, T, d, NX, NY, true); }

  { std::vector<bool> d((size_t)NX * NY, false);
    for (int ty = 0; ty < NY; ty++) d[(size_t)ty * NX + (NX - 1)] = true;
    run("whole right column", W, H, T, d, NX, NY); }

  /* 5. sizes that are NOT whole tiles -- 210x150 leaves partial tiles on two
     edges, which is where an off-by-one would actually land */
  for (int w = 200; w <= 216; w += 4)
    for (int h = 146; h <= 154; h += 4) {
      int nx = (w + T - 1) / T, ny = (h + T - 1) / T;
      std::vector<bool> d((size_t)nx * ny, true);
      char nm[64]; snprintf(nm, sizeof(nm), "ragged %dx%d, all dirty", w, h);
      run(nm, w, h, T, d, nx, ny);
    }

  /* randomised: 400 patterns, every invariant on each */
  std::mt19937 rng(20260904);
  for (int trial = 0; trial < 400; trial++) {
    int w = 64 + (int)(rng() % 300), h = 48 + (int)(rng() % 240);
    int nx = (w + T - 1) / T, ny = (h + T - 1) / T;
    std::vector<bool> d((size_t)nx * ny);
    for (size_t i = 0; i < d.size(); i++) d[i] = (rng() % 100) < 35;
    std::vector<Rect> rects;
    tileRuns(w, h, T,
      [&](int tx, int ty) { return d[(size_t)ty * nx + tx]; },
      [&](int x, int y, int rw, int rh) { rects.push_back({x, y, rw, rh}); });
    std::vector<int> cover((size_t)w * h, 0);
    for (auto& r : rects) {
      if (r.x < 0 || r.y < 0 || r.x + r.w > w || r.y + r.h > h || r.w <= 0 || r.h <= 0) {
        printf("  FAIL  random trial %d: rect out of bounds\n", trial); failures++; break;
      }
      for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++) cover[(size_t)y * w + x]++;
    }
    for (int ty = 0; ty < ny && !failures; ty++)
      for (int tx = 0; tx < nx; tx++) {
        bool dd = d[(size_t)ty * nx + tx];
        int x0 = tx * T, y0 = ty * T;
        int x1 = (x0 + T > w) ? w : x0 + T, y1 = (y0 + T > h) ? h : y0 + T;
        for (int y = y0; y < y1; y++)
          for (int x = x0; x < x1; x++)
            if (cover[(size_t)y * w + x] != (dd ? 1 : 0)) {
              printf("  FAIL  random trial %d: bad coverage at %d,%d\n", trial, x, y);
              failures++; goto next;
            }
      }
    next:;
  }
  printf("\n400 random patterns + fixed cases: %s\n",
         failures ? "FAILURES" : "every invariant held");
  return failures ? 1 : 0;
}
