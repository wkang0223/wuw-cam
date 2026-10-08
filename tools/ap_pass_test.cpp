// Host test for the access-point password rules.
//   g++ -std=c++17 -Wall -Wextra -I. tools/ap_pass_test.cpp -o /tmp/ap_pass_test && /tmp/ap_pass_test
#include <cstdio>
#include <string>
#include <strings.h>
#include "ap_pass.h"

static int failures = 0;
static void expect(const std::string& p, ApPassCheck want) {
  ApPassCheck got = apPassCheck(p.c_str(), p.size());
  bool ok = got == want;
  std::printf("%s  \"%s\" -> %s%s\n", ok ? "PASS" : "FAIL", p.size() > 40 ? (p.substr(0, 37) + "...").c_str() : p.c_str(),
              apPassCheckText(got), ok ? "" : "  (wanted different)");
  if (!ok) ++failures;
}

int main() {
  expect("moonrabbit-42", AP_PASS_OK);
  expect("correct horse battery", AP_PASS_OK);          // spaces inside are fine
  expect("Zx9!Zx9!", AP_PASS_OK);                       // exactly 8
  expect(std::string(63, 'a') + "", AP_PASS_GUESSABLE); // one repeated character
  expect(std::string(31, 'a') + "b" + std::string(31, 'a'), AP_PASS_OK);   // exactly 63
  expect(std::string(64, 'a'), AP_PASS_TOO_LONG);
  expect("short7!", AP_PASS_TOO_SHORT);
  expect("", AP_PASS_TOO_SHORT);
  expect("wuwuwuwu", AP_PASS_GUESSABLE);                // the factory password is not "custom"
  expect("WUWUWUWU", AP_PASS_GUESSABLE);                // in any case
  expect("12345678", AP_PASS_GUESSABLE);
  expect("Password", AP_PASS_GUESSABLE);
  expect("pass\tword99", AP_PASS_BAD_CHAR);
  expect("caf\xc3\xa9-bar99", AP_PASS_BAD_CHAR);        // non-ASCII bytes
  expect(" leadingspace1", AP_PASS_SPACES);
  expect("trailingspace1 ", AP_PASS_SPACES);
  std::printf("%d failure(s)\n", failures);
  return failures ? 1 : 0;
}
