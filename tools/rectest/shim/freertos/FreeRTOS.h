#pragma once
#include "../Arduino.h"
#include <mutex>
#include <condition_variable>
#include <deque>
#include <pthread.h>
typedef void* TaskHandle_t; typedef int BaseType_t; typedef uint32_t TickType_t;
#define portTICK_PERIOD_MS 1
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(x) ((TickType_t)(x))
#define pdTRUE 1
#define pdFALSE 0
