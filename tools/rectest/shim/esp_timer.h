#pragma once
#include "Arduino.h"
static inline int64_t esp_timer_get_time() { return (int64_t)micros(); }
