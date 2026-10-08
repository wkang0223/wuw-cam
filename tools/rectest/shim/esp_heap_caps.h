#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static inline void* heap_caps_malloc(size_t n, int) { return malloc(n); }
static inline void heap_caps_free(void* p) { free(p); }
static inline size_t heap_caps_get_free_size(int) { return 6u * 1024 * 1024; }
