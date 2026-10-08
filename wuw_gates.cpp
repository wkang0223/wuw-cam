/* Eight Gates camera ritual.
 *
 * Analysis deliberately uses a 20x12 sample grid. The sensor and JPEG decoder
 * already did the expensive work; these 240 samples are enough to distinguish
 * the ritual conditions without adding another full-frame allocation. */
#include "wuw_gates.h"

#include <math.h>
#include <string.h>

namespace {

constexpr int GRID_W = 20;
constexpr int GRID_H = 12;
constexpr int GRID_N = GRID_W * GRID_H;
static uint8_t previous[GRID_N];
static bool previousValid = false;

static uint8_t clampByte(int value) {
  return (uint8_t)constrain(value, 0, 255);
}

static uint8_t scaledScore(int value, int low, int high) {
  if (high <= low) return value >= high ? 100 : 0;
  return clampByte((value - low) * 100 / (high - low));
}

}  // namespace

void wuwGatesResetAnalysis() {
  memset(previous, 0, sizeof(previous));
  previousValid = false;
}

WuwGateMetrics wuwGatesAnalyze(const uint16_t* pixels, int width, int height,
                               int stride) {
  WuwGateMetrics out = {};
  if (!pixels || width < 2 || height < 2 || stride < width) return out;

  uint8_t current[GRID_N];
  uint32_t sumR = 0, sumG = 0, sumB = 0, sumL = 0, sumL2 = 0;
  uint32_t edgeSum = 0, edgeN = 0, motionSum = 0;
  uint16_t zoneMotion[12] = {};
  uint16_t zoneSamples[12] = {};

  for (int gy = 0; gy < GRID_H; ++gy) {
    int sy = ((gy * 2 + 1) * height) / (GRID_H * 2);
    sy = constrain(sy, 0, height - 1);
    for (int gx = 0; gx < GRID_W; ++gx) {
      int sx = ((gx * 2 + 1) * width) / (GRID_W * 2);
      sx = constrain(sx, 0, width - 1);
      uint16_t p = pixels[(size_t)sy * stride + sx];
      uint8_t r = (uint8_t)(((p >> 11) & 31) * 255 / 31);
      uint8_t green = (uint8_t)(((p >> 5) & 63) * 255 / 63);
      uint8_t b = (uint8_t)((p & 31) * 255 / 31);
      uint8_t l = (uint8_t)((77 * r + 150 * green + 29 * b) >> 8);
      int i = gy * GRID_W + gx;
      current[i] = l;
      sumR += r; sumG += green; sumB += b; sumL += l; sumL2 += (uint32_t)l * l;

      if (gx) { edgeSum += abs((int)l - current[i - 1]); ++edgeN; }
      if (gy) { edgeSum += abs((int)l - current[i - GRID_W]); ++edgeN; }
      if (previousValid) {
        uint8_t d = (uint8_t)abs((int)l - previous[i]);
        motionSum += d;
        int zone = (gy / 4) * 4 + (gx / 5);
        zoneMotion[zone] += d;
        ++zoneSamples[zone];
      }
    }
  }

  out.red = (uint8_t)(sumR / GRID_N);
  out.green = (uint8_t)(sumG / GRID_N);
  out.blue = (uint8_t)(sumB / GRID_N);
  out.luma = (uint8_t)(sumL / GRID_N);
  uint32_t meanSquare = sumL2 / GRID_N;
  uint32_t squareMean = (sumL / GRID_N) * (sumL / GRID_N);
  out.contrast = clampByte((int)sqrtf((float)(meanSquare > squareMean
                                             ? meanSquare - squareMean : 0)));
  out.edge = edgeN ? clampByte((int)(edgeSum / edgeN)) : 0;
  out.motion = previousValid ? clampByte((int)(motionSum / GRID_N)) : 0;
  for (int i = 0; i < 12; ++i) {
    if (previousValid && zoneSamples[i] &&
        zoneMotion[i] / zoneSamples[i] >= 18) ++out.motionZones;
  }

  memcpy(previous, current, sizeof(previous));
  previousValid = true;
  return out;
}

const char* wuwGateName(uint8_t gate) {
  static const char* names[WUW_GATE_COUNT] = {
    "VOID", "BLOOD", "VEIL", "GHOST",
    "TIDE", "THORN", "NOISE", "CHORUS",
  };
  return names[gate % WUW_GATE_COUNT];
}

const char* wuwGatePrompt(uint8_t gate) {
  static const char* prompts[WUW_GATE_COUNT] = {
    "COVER THE LENS", "FIND RED", "FACE THE LIGHT", "MOVE THROUGH FRAME",
    "FIND CYAN OR BLUE", "FIND DENSE EDGES", "FIND DARK AND LIGHT", "MOVE TOGETHER",
  };
  return prompts[gate % WUW_GATE_COUNT];
}

uint8_t wuwGateScore(uint8_t gate, const WuwGateMetrics& m, int visitors) {
  switch (gate % WUW_GATE_COUNT) {
    case 0: return scaledScore(90 - m.luma, 20, 60);                    // <=30
    case 1: return min(scaledScore(m.red, 65, 125),
                       scaledScore((int)m.red - max(m.green, m.blue), 5, 32));
    case 2: return scaledScore(m.luma, 105, 178);
    case 3: return scaledScore(m.motion, 5, 24);
    case 4: return min(scaledScore(m.blue, 60, 120),
                       scaledScore((int)m.blue + m.green / 2 - m.red, 30, 95));
    case 5: return scaledScore(m.edge, 8, 30);
    case 6: return scaledScore(m.contrast, 20, 62);
    case 7:
      if (visitors >= 2) return 100;
      return scaledScore(m.motionZones, 1, 6);
    default: return 0;
  }
}

