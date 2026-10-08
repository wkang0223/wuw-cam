/* See WuwEventBridge.h. One-way UDP announcements, nothing received. */
#ifdef WUW_EVENT_BRIDGE

#include "WuwEventBridge.h"
#include <WiFi.h>
#include <WiFiUdp.h>

static WiFiUDP  udp;
static bool     up = false;
static int      lastCount = -1;
static uint32_t lastActivity = 0;
static bool     idleSent = false;

#define WUW_IDLE_MS (120000UL)     // two quiet minutes before we say so

static void send(const char* line) {
  if (!up) return;
  /* Broadcast to the AP subnet. XEN-T1 joins WUW-01 as a station, so this
     reaches it without either device knowing the other's address. */
  IPAddress bc = WiFi.softAPIP();
  bc[3] = 255;
  if (udp.beginPacket(bc, WUW_EVENT_PORT)) {
    udp.write((const uint8_t*)line, strlen(line));
    udp.endPacket();
  }
}

static void mark() { lastActivity = millis(); idleSent = false; }

void wuwBridgeBegin() {
  up = udp.begin(WUW_EVENT_PORT) == 1;
  lastActivity = millis();
  Serial.printf("[BRIDGE] UDP %s on port %d\n", up ? "up" : "FAILED", WUW_EVENT_PORT);
}

void wuwBridgeBoot() { mark(); send("WUW|BOOT"); }

void wuwBridgeClientCount(int n) {
  if (n == lastCount) return;               // never repeat an unchanged state
  lastCount = n;
  mark();
  char b[32];
  snprintf(b, sizeof(b), "WUW|CLIENT_COUNT|%d", n);
  send(b);
}

void wuwBridgeJoin(const char* alias) {
  mark();
  char b[64];
  snprintf(b, sizeof(b), "WUW|JOIN|%.32s", alias ? alias : "visitor");
  send(b);
}

void wuwBridgeShutter(uint32_t img, const char* alias) {
  mark();
  char b[72];
  snprintf(b, sizeof(b), "WUW|SHUTTER|%04u|%.32s", (unsigned)img,
           alias ? alias : "visitor");
  send(b);
}

void wuwBridgePhotoSaved(uint32_t img) {
  mark();
  char b[40];
  snprintf(b, sizeof(b), "WUW|PHOTO_SAVED|%04u", (unsigned)img);
  send(b);
}

void wuwBridgeNote(uint32_t img) {
  mark();
  char b[32];
  snprintf(b, sizeof(b), "WUW|NOTE|%04u", (unsigned)img);
  send(b);
}

void wuwBridgeTick() {
  if (!up || idleSent) return;
  if (millis() - lastActivity > WUW_IDLE_MS) {
    idleSent = true;                        // said once, not repeatedly
    send("WUW|IDLE");
  }
}

#endif
