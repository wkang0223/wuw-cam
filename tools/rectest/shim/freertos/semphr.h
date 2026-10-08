#pragma once
#include "FreeRTOS.h"
typedef std::recursive_mutex* SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new std::recursive_mutex; }
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t) { m->lock(); return pdTRUE; }
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t m) { m->unlock(); return pdTRUE; }
