#include "ui_art.h"
#include <Arduino.h>
#include <strings.h>

const uint8_t WUW_UI_ART_JPG[] PROGMEM = {
#include "assets/wuw-skin-nacre.inc"
};

const size_t WUW_UI_ART_JPG_LEN = sizeof(WUW_UI_ART_JPG);

static const uint8_t ART_GUNMETAL[] PROGMEM = {
#include "assets/wuw-skin-gunmetal.inc"
};
static const uint8_t ART_OBSIDIAN[] PROGMEM = {
#include "assets/wuw-skin-obsidian.inc"
};
static const uint8_t ART_EMBER[] PROGMEM = {
#include "assets/wuw-skin-ember.inc"
};
static const uint8_t ART_SEANCE[] PROGMEM = {
#include "assets/wuw-skin-seance.inc"
};
static const uint8_t ART_HANGAR[] PROGMEM = {
#include "assets/wuw-skin-hangar.inc"
};
static const uint8_t ART_CHASSIS[] PROGMEM = {
#include "assets/wuw-skin-chassis.inc"
};
static const uint8_t ART_SIGIL[] PROGMEM = {
#include "assets/wuw-skin-sigil.inc"
};
static const uint8_t ART_RELIQUARY[] PROGMEM = {
#include "assets/wuw-skin-reliquary.inc"
};
static const uint8_t ART_FAIRY[] PROGMEM = {
#include "assets/wuw-skin-fairy.inc"
};

const uint8_t* wuwUiArtForSkin(const char* skin, size_t* len) {
  const uint8_t* data = WUW_UI_ART_JPG;
  size_t size = WUW_UI_ART_JPG_LEN;
  if (skin) {
    if (!strcasecmp(skin, "gunmetal")) {
      data = ART_GUNMETAL; size = sizeof(ART_GUNMETAL);
    } else if (!strcasecmp(skin, "obsidian")) {
      data = ART_OBSIDIAN; size = sizeof(ART_OBSIDIAN);
    } else if (!strcasecmp(skin, "ember")) {
      data = ART_EMBER; size = sizeof(ART_EMBER);
    } else if (!strcasecmp(skin, "seance")) {
      data = ART_SEANCE; size = sizeof(ART_SEANCE);
    } else if (!strcasecmp(skin, "hangar")) {
      data = ART_HANGAR; size = sizeof(ART_HANGAR);
    } else if (!strcasecmp(skin, "chassis")) {
      data = ART_CHASSIS; size = sizeof(ART_CHASSIS);
    } else if (!strcasecmp(skin, "sigil")) {
      data = ART_SIGIL; size = sizeof(ART_SIGIL);
    } else if (!strcasecmp(skin, "reliquary")) {
      data = ART_RELIQUARY; size = sizeof(ART_RELIQUARY);
    } else if (!strcasecmp(skin, "fairy")) {
      data = ART_FAIRY; size = sizeof(ART_FAIRY);
    }
  }
  if (len) *len = size;
  return data;
}
