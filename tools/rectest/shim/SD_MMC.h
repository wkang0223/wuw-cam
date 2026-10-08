#pragma once
#include "Arduino.h"
// A card that stalls the way FAT does: every Nth frame-sized write blocks.
extern volatile int g_sdStallEvery, g_sdStallMs; extern volatile uint32_t g_sdWrites;
#define FILE_WRITE "wb"
struct File {
  FILE* f = nullptr;
  operator bool() const { return f != nullptr; }
  size_t write(uint8_t b) { return f ? fwrite(&b, 1, 1, f) : 0; }
  size_t write(const uint8_t* p, size_t n) {
    if (!f) return 0;
    if (n > 4096 && g_sdStallEvery && (++g_sdWrites % g_sdStallEvery) == 0) delay(g_sdStallMs);
    return fwrite(p, 1, n, f); }
  size_t position() { return f ? (size_t)ftell(f) : 0; }
  bool seek(size_t p) { return f && fseek(f, (long)p, SEEK_SET) == 0; }
  void close() { if (f) { fclose(f); f = nullptr; } }
};
struct SDStub { File open(const char* p, const char* m) { File x; x.f = fopen(p, m); return x; } };
extern SDStub SD_MMC;
