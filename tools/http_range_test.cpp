// Host test for the byte-range parser.
//   g++ -std=c++17 -Wall -Wextra -I. tools/http_range_test.cpp -o /tmp/http_range_test && /tmp/http_range_test
#include <cstdio>
#include "http_range.h"

static int failures = 0;
static void expect(const char* header, size_t total, RangeParse want, size_t wf = 0, size_t wl = 0) {
  size_t f = 0, l = 0;
  RangeParse got = parseByteRangeHeader(header, total, &f, &l);
  bool ok = got == want && (want != RANGE_OK || (f == wf && l == wl));
  std::printf("%s  %-22s total=%-7zu -> %d", ok ? "PASS" : "FAIL", header ? header : "(none)", total, (int)got);
  if (got == RANGE_OK) std::printf(" %zu-%zu", f, l);
  std::printf("\n");
  if (!ok) ++failures;
}

int main() {
  expect(nullptr,           1000, RANGE_NONE);
  expect("bytes=0-1",       1000, RANGE_OK, 0, 1);              // the probe Safari sends first
  expect("bytes=0-",        1000, RANGE_OK, 0, 999);
  expect("bytes=500-",      1000, RANGE_OK, 500, 999);
  expect("bytes=100-199",   1000, RANGE_OK, 100, 199);
  expect("bytes=900-5000",  1000, RANGE_OK, 900, 999);          // end past the file is clamped
  expect("bytes=-100",      1000, RANGE_OK, 900, 999);          // suffix
  expect("bytes=-5000",     1000, RANGE_OK, 0, 999);            // suffix longer than the file
  expect("bytes=1000-",     1000, RANGE_BAD);                   // starts past the end
  expect("bytes=0-0",       1000, RANGE_OK, 0, 0);
  expect("bytes=-0",        1000, RANGE_BAD);
  expect("bytes=5-2",       1000, RANGE_NONE);                  // inverted: ignore
  expect("bytes=0-1,5-6",   1000, RANGE_NONE);                  // multiple ranges: ignore
  expect("items=0-1",       1000, RANGE_NONE);                  // other unit
  expect("bytes=abc",       1000, RANGE_NONE);
  expect("bytes=",          1000, RANGE_NONE);
  expect("bytes=0-1",       0,    RANGE_BAD);                   // empty file
  std::printf("%d failure(s)\n", failures);
  return failures ? 1 : 0;
}
