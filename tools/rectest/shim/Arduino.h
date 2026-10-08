#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <chrono>
#include <thread>
#include <algorithm>
static inline uint32_t millis() {
  using namespace std::chrono; static auto t0 = steady_clock::now();
  return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - t0).count(); }
static inline uint32_t micros() {
  using namespace std::chrono; static auto t0 = steady_clock::now();
  return (uint32_t)duration_cast<microseconds>(steady_clock::now() - t0).count(); }
static inline void delay(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
class String {
 public:
  std::string s;
  String() {}
  String(const char* c) : s(c ? c : "") {}
  String(unsigned long v) : s(std::to_string(v)) {}
  String(unsigned int v) : s(std::to_string(v)) {}
  String(int v) : s(std::to_string(v)) {}
  String(float v, int d) { char b[32]; snprintf(b, sizeof b, "%.*f", d, v); s = b; }
  size_t length() const { return s.size(); }
  const char* c_str() const { return s.c_str(); }
  String operator+(const String& o) const { String r; r.s = s + o.s; return r; }
  String operator+(const char* o) const { String r; r.s = s + o; return r; }
  String& operator+=(const String& o) { s += o.s; return *this; }
  String& operator+=(const char* o) { s += o; return *this; }
};
inline String operator+(const char* a, const String& b) { String r(a); r.s += b.s; return r; }
struct SerialStub { void println(const char* x) { printf("%s\n", x); }
  template<typename... A> void printf(const char* f, A... a) { ::printf(f, a...); } };
extern SerialStub Serial;
#include <stdlib.h>
inline long random(long m) { return m > 0 ? (long)(rand() % m) : 0; }
