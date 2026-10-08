#pragma once
/* HTTP byte ranges ("Range: bytes=a-b"), as pure C++ so the parsing can be tested on a computer
 * (tools/http_range_test.cpp).
 *
 * Why the camera needs this: iPhone Safari will not play a video, nor offer to save it into
 * Photos, unless the server can answer "give me bytes a to b" with a 206 Partial Content. A
 * server that always replies 200 with the whole file leaves Safari showing a dead player, and the
 * only thing left is a download into the Files app. */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum RangeParse : unsigned char {
  RANGE_NONE = 0,   // no usable Range header: send the whole thing with 200
  RANGE_OK   = 1,   // serve bytes first..last (inclusive) with 206
  RANGE_BAD  = 2,   // nothing in the file matches: answer 416
};

/* `header` is the Range header value (or null). Only a single "bytes=" range is honoured; other
   units, multiple ranges and malformed values are ignored, which the HTTP rules allow. */
inline RangeParse parseByteRangeHeader(const char* header, size_t total, size_t* first, size_t* last) {
  if (!header || strncmp(header, "bytes=", 6) != 0 || strchr(header, ',')) return RANGE_NONE;
  const char* p = header + 6;
  if (!total) return RANGE_BAD;
  size_t a, b;
  if (*p == '-') {                                      // "-N": the last N bytes
    char* e = nullptr;
    unsigned long n = strtoul(p + 1, &e, 10);
    if (e == p + 1 || *e != 0) return RANGE_NONE;
    if (!n) return RANGE_BAD;
    a = (size_t)n >= total ? 0 : total - (size_t)n;
    b = total - 1;
  } else {
    char* e = nullptr;
    unsigned long first_ = strtoul(p, &e, 10);
    if (e == p || *e != '-') return RANGE_NONE;
    a = (size_t)first_;
    if (e[1]) {
      char* e2 = nullptr;
      unsigned long last_ = strtoul(e + 1, &e2, 10);
      if (e2 == e + 1 || *e2 != 0) return RANGE_NONE;
      b = (size_t)last_;
    } else {
      b = total - 1;                                    // "a-": to the end
    }
    if (a >= total) return RANGE_BAD;
    if (b >= total) b = total - 1;
    if (b < a) return RANGE_NONE;
  }
  *first = a; *last = b;
  return RANGE_OK;
}
