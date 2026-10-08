#pragma once

#include <stddef.h>
#include <stdint.h>

extern const uint8_t WUW_UI_ART_JPG[];
extern const size_t WUW_UI_ART_JPG_LEN;

/* Returns the flash-resident JPEG for a web skin. Unknown names fall back to
   Nacre so a stale browser preference can never produce a broken border. */
const uint8_t* wuwUiArtForSkin(const char* skin, size_t* len);
