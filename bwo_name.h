#pragma once
// The pilgrim's name: what the player calls their BWO character. Pure C++ with no Arduino
// dependency so the rules can be tested on a computer (tools/bwo_name_test.cpp). The browser
// preview (site/game-preview/pilgrim-name.js) applies the same rules.
#include <stddef.h>

#define BWO_NAME_MAX 12
#define BWO_NAME_DEFAULT "Q"

// A-Z, 0-9, space, hyphen, underscore and full stop. Anything else is dropped.
inline char bwoNameUpper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

inline bool bwoNameCharOk(char c) {
  c = bwoNameUpper(c);
  return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
         c == ' ' || c == '-' || c == '_' || c == '.';
}

// Writes the cleaned name into `out` (BWO_NAME_MAX + 1 bytes or more) and returns its length:
// upper-cased, disallowed characters dropped, spaces trimmed from both ends and never doubled,
// at most BWO_NAME_MAX long. If nothing is left (or `in` is null) the result is BWO_NAME_DEFAULT.
inline size_t bwoNameClean(const char* in, char* out) {
  size_t n = 0;
  for (const char* p = in; p && *p && n < BWO_NAME_MAX; ++p) {
    char c = bwoNameUpper(*p);
    if (!bwoNameCharOk(c)) continue;
    if (c == ' ' && (n == 0 || out[n - 1] == ' ')) continue;
    out[n++] = c;
  }
  while (n && out[n - 1] == ' ') --n;
  if (!n) {
    const char* d = BWO_NAME_DEFAULT;
    while (*d && n < BWO_NAME_MAX) out[n++] = *d++;
  }
  out[n] = 0;
  return n;
}
