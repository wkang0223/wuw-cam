#pragma once
#include "FreeRTOS.h"
struct QueueImpl { std::mutex m; std::condition_variable cv; std::deque<std::string> q; size_t item; size_t cap; };
typedef QueueImpl* QueueHandle_t;
static inline QueueHandle_t xQueueCreate(size_t len, size_t item) { auto* q = new QueueImpl; q->item = item; q->cap = len; return q; }
static inline void vQueueDelete(QueueHandle_t q) { delete q; }
static inline BaseType_t xQueueSend(QueueHandle_t q, const void* p, TickType_t) {
  std::lock_guard<std::mutex> g(q->m); if (q->q.size() >= q->cap) return pdFALSE;
  q->q.emplace_back((const char*)p, q->item); q->cv.notify_one(); return pdTRUE; }
static inline BaseType_t xQueueReceive(QueueHandle_t q, void* p, TickType_t t) {
  std::unique_lock<std::mutex> g(q->m);
  if (q->q.empty() && t) q->cv.wait_for(g, std::chrono::milliseconds(t == portMAX_DELAY ? 1000000 : t), [&]{ return !q->q.empty(); });
  if (q->q.empty()) return pdFALSE; memcpy(p, q->q.front().data(), q->item); q->q.pop_front(); return pdTRUE; }
static inline uint32_t uxQueueMessagesWaiting(QueueHandle_t q) { std::lock_guard<std::mutex> g(q->m); return (uint32_t)q->q.size(); }
