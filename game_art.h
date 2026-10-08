#pragma once
#include <stdint.h>

// Shared 32px tiles live in flash; zero is transparent in the eight sprites.
namespace WuwGameArt {
constexpr int SIZE = 32;
constexpr int COUNT = 48;
extern const uint16_t pixels[COUNT * SIZE * SIZE];
inline uint16_t sample(int tile, int x, int y) {
  return pixels[(tile % COUNT) * SIZE * SIZE + (y & 31) * SIZE + (x & 31)];
}
}
