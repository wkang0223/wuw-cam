#pragma once

#include <Arduino.h>

#include "bwo_name.h"

// The game renders at half panel width. panel.cpp expands each pixel 2x so
// the raycaster stays inexpensive while filling the complete 320x168 OS body.
#define BWO_FB_W 160
#define BWO_FB_H 84
#define BWO_WALL_TEXTURE_SLOTS 4
#define BWO_LEVELS 3

enum BwoFace : uint8_t {
  BWO_FACE_NORTH = 0, BWO_FACE_EAST, BWO_FACE_SOUTH,
  BWO_FACE_WEST, BWO_FACE_FLOOR, BWO_FACE_CEILING,
};

bool bwoGameBegin(bool sdAvailable);
void bwoGameReset();

// The player's name for their pilgrim (rules in bwo_name.h): never empty, at most BWO_NAME_MAX
// characters, "Q" until they pick one. It is kept in flash, so it survives a reboot and a world reset.
const char* bwoGameName();
bool bwoGameSetName(const char* name);   // cleans it first; false if flash would not take it
void bwoGameTouch(int x, int y, bool down);
void bwoGameFire();
void bwoGameTogglePause();
bool bwoGamePaused();
bool bwoGameReady();
void bwoGameToggleBuild();
bool bwoGameBuildMode();
bool bwoGamePlaceWall();
bool bwoGameRemoveWall();
bool bwoGameSavePatch(uint8_t slot);
bool bwoGameLoadPatch(uint8_t slot);
bool bwoGamePatchExists(uint8_t slot);
uint8_t bwoGameTheme();
bool bwoGameCycleTheme();
uint8_t bwoGameUv();
void bwoGameCycleUv();
uint8_t bwoGameGlitch();
void bwoGameCycleGlitch();
uint8_t bwoGameFragments();
void bwoGameCycleWallTexture();
bool bwoGameSelectWallTexture(uint8_t slot);
uint8_t bwoGameSelectedWallTexture();
uint8_t bwoGameLevel();
bool bwoGameChangeLevel(int direction);
void bwoGameCycleFace();
BwoFace bwoGameSelectedFace();
bool bwoGamePaintFace();

// Copies a gallery image into the next 32x32 wall slot. The caller retains
// ownership of pixels; the game keeps only an 8 KB maximum texture atlas.
bool bwoGameAddWallTexture(const uint16_t* pixels, int width, int height,
                           int stride);
void bwoGameClearWallTextures();
uint8_t bwoGameWallTextureCount();
const uint16_t* bwoGameWallTexture(uint8_t slot);

// Advances the fixed-rate simulation and returns the current RGB565 frame.
// The pointer remains owned by the game and is valid until the next call.
const uint16_t* bwoGameFrame(uint32_t nowMs);
