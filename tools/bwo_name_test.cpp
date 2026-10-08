// Host test for the pilgrim-name rules.
//   g++ -std=c++17 -Wall -Wextra -I. tools/bwo_name_test.cpp -o /tmp/bwo_name_test && /tmp/bwo_name_test
#include <cstdio>
#include <cstring>

#include "bwo_name.h"

static int failures = 0;

static void expect(const char* in, const char* want) {
  char out[BWO_NAME_MAX + 1];
  size_t n = bwoNameClean(in, out);
  bool ok = strcmp(out, want) == 0 && n == strlen(want) && n <= BWO_NAME_MAX;
  std::printf("%s  \"%s\" -> \"%s\"%s\n", ok ? "PASS" : "FAIL", in ? in : "(null)", out, ok ? "" : " (wanted different)");
  if (!ok) { std::printf("      wanted \"%s\"\n", want); ++failures; }
}

int main() {
  expect("mika", "MIKA");                       // lower case becomes upper case
  expect("W8I", "W8I");                        // any valid name stays as typed
  expect("  moon  rabbit  ", "MOON RABBIT");    // trimmed, no doubled spaces
  expect("a-b_c.d", "A-B_C.D");                 // the allowed marks survive
  expect("ZERO<script>", "ZEROSCRIPT");         // everything else is dropped
  expect("naïve", "NAVE");                      // non-ASCII bytes are dropped
  expect("ABCDEFGHIJKLMNOP", "ABCDEFGHIJKL");   // at most 12
  expect("ABCDEFGHIJK LMN", "ABCDEFGHIJK");     // a space that would end the 12 is trimmed
  expect("", BWO_NAME_DEFAULT);                 // nothing left: the default
  expect("   ", BWO_NAME_DEFAULT);
  expect("%%%", BWO_NAME_DEFAULT);
  expect(nullptr, BWO_NAME_DEFAULT);
  // the key filter the keyboard uses
  bool filter = bwoNameCharOk('a') && bwoNameCharOk('Z') && bwoNameCharOk('7') && bwoNameCharOk(' ') &&
                bwoNameCharOk('-') && bwoNameCharOk('_') && bwoNameCharOk('.') &&
                !bwoNameCharOk('@') && !bwoNameCharOk('"') && !bwoNameCharOk('\n') && !bwoNameCharOk(0x7f);
  std::printf("%s  key filter\n", filter ? "PASS" : "FAIL");
  if (!filter) ++failures;
  std::printf("%d failure(s)\n", failures);
  return failures ? 1 : 0;
}
