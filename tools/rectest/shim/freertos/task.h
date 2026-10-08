#pragma once
#include "FreeRTOS.h"
static inline void vTaskDelay(TickType_t t) { delay(t); }
static inline void vTaskDelete(TaskHandle_t) { pthread_exit(nullptr); }
typedef void (*TaskFn)(void*);
static inline BaseType_t xTaskCreatePinnedToCore(TaskFn fn, const char*, uint32_t, void* arg, int, TaskHandle_t* h, int) {
  static int token = 1; if (h) *h = (void*)(intptr_t)(token++);      // set BEFORE the task can finish
  std::thread([fn, arg]() { fn(arg); }).detach(); return pdTRUE; }
