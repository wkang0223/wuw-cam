#include "game_art.h"
#include <Arduino.h>
namespace WuwGameArt {
const uint16_t pixels[COUNT * SIZE * SIZE] PROGMEM = {
#include "assets/wuw-rpg-atlas.inc"
};
}
