#pragma once
/* Rules for the camera's own WiFi password, as pure C++ so they can be tested on a computer
 * (tools/ap_pass_test.cpp). WPA2 needs 8 to 63 printable ASCII characters; on top of that this
 * refuses the passwords that are printed in the manual or that anyone tries first, because the
 * point of letting each owner set their own is that a stranger cannot guess it. */
#include <stddef.h>
#include <string.h>

#define AP_PASS_MIN 8
#define AP_PASS_MAX 63
#define AP_PASS_FACTORY "wuwuwuwu"

enum ApPassCheck {
  AP_PASS_OK = 0,
  AP_PASS_TOO_SHORT,
  AP_PASS_TOO_LONG,
  AP_PASS_BAD_CHAR,
  AP_PASS_SPACES,      // starts or ends with a space: invisible on a phone, impossible to type back
  AP_PASS_GUESSABLE,   // the factory password, a repeated character or a common one
};

inline ApPassCheck apPassCheck(const char* p, size_t n) {
  if (!p || n < AP_PASS_MIN) return AP_PASS_TOO_SHORT;
  if (n > AP_PASS_MAX) return AP_PASS_TOO_LONG;
  bool same = true;
  for (size_t i = 0; i < n; ++i) {
    unsigned char c = (unsigned char)p[i];
    if (c < 0x20 || c > 0x7E) return AP_PASS_BAD_CHAR;
    if (c != (unsigned char)p[0]) same = false;
  }
  if (p[0] == ' ' || p[n - 1] == ' ') return AP_PASS_SPACES;
  if (same) return AP_PASS_GUESSABLE;
  static const char* const common[] = { AP_PASS_FACTORY, "12345678", "123456789", "1234567890",
                                        "password", "password1", "qwertyui", "87654321", "wuwcam123" };
  for (size_t i = 0; i < sizeof(common) / sizeof(common[0]); ++i)
    if (strlen(common[i]) == n && strncasecmp(common[i], p, n) == 0) return AP_PASS_GUESSABLE;
  return AP_PASS_OK;
}

inline const char* apPassCheckText(ApPassCheck c) {
  switch (c) {
    case AP_PASS_OK:         return "ok";
    case AP_PASS_TOO_SHORT:  return "needs at least 8 characters";
    case AP_PASS_TOO_LONG:   return "at most 63 characters";
    case AP_PASS_BAD_CHAR:   return "letters, numbers and ordinary symbols only";
    case AP_PASS_SPACES:     return "cannot start or end with a space";
    case AP_PASS_GUESSABLE:  return "too easy to guess - choose something of your own";
  }
  return "invalid";
}
