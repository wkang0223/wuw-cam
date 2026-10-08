#pragma once

#include <Arduino.h>

#define WUW_GATE_COUNT 8

struct WuwGateMetrics {
  uint8_t luma;
  uint8_t red;
  uint8_t green;
  uint8_t blue;
  uint8_t contrast;
  uint8_t edge;
  uint8_t motion;
  uint8_t motionZones;
};

void wuwGatesResetAnalysis();
WuwGateMetrics wuwGatesAnalyze(const uint16_t* pixels, int width, int height,
                               int stride);
const char* wuwGateName(uint8_t gate);
const char* wuwGatePrompt(uint8_t gate);
uint8_t wuwGateScore(uint8_t gate, const WuwGateMetrics& metrics,
                     int visitors);

