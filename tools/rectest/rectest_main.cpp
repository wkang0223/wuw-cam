// Drives the REAL video_writer.cpp against a simulated camera and card.
#include "Arduino.h"
#include "esp_camera.h"
#include "SD_MMC.h"
#include "video_writer.h"
#include <vector>
#include <atomic>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <fstream>
#include <iterator>

SerialStub Serial; SDStub SD_MMC;
volatile int g_sdStallEvery = 0, g_sdStallMs = 0; volatile uint32_t g_sdWrites = 0;
static int fakeSat(sensor_t*, int) { return 0; }
static sensor_t g_sensor = { fakeSat, {} };
sensor_t* esp_camera_sensor_get() { return &g_sensor; }

// ---- the simulated sensor: a frame goes to exactly ONE caller ----
struct Produced { int idx; uint64_t us; };
static std::mutex qm; static std::condition_variable qcv; static std::deque<Produced> cq;
static std::vector<std::vector<uint8_t>> jpgs;
static std::atomic<bool> prodRun{false}; static std::atomic<int> producedN{0};
static FILE* prodLog = nullptr;
static void producer() {
  uint64_t t = micros(); int i = 0;
  while (prodRun) {
    int interval = 40000 + (int)((i * 7919) % 6001) - 3000;          // 40 ms +/- 3 ms of jitter
    if (i == 60 || i == 140) interval += 80000;                      // two long gaps
    t += interval;
    while (micros() < t && prodRun) std::this_thread::sleep_for(std::chrono::microseconds(500));
    if (!prodRun) break;
    { std::lock_guard<std::mutex> g(qm);
      if (cq.size() >= 2) cq.pop_front();                            // latest-wins, like the driver
      cq.push_back({i, t}); }
    if (prodLog) fprintf(prodLog, "%d %llu\n", i, (unsigned long long)t);
    ++producedN; ++i; qcv.notify_one();
  }
}
camera_fb_t* esp_camera_fb_get() {
  std::unique_lock<std::mutex> g(qm);
  if (!qcv.wait_for(g, std::chrono::milliseconds(300), []{ return !cq.empty(); })) return nullptr;
  Produced p = cq.front(); cq.pop_front();
  auto* fb = new camera_fb_t();
  auto& j = jpgs[p.idx % jpgs.size()];
  fb->buf = j.data(); fb->len = j.size(); fb->width = 800; fb->height = 600;
  fb->timestamp.tv_sec = (long)(p.us / 1000000); fb->timestamp.tv_usec = (long)(p.us % 1000000);
  return fb;
}
void esp_camera_fb_return(camera_fb_t* fb) { delete fb; }

static std::atomic<int> viewerGot{0}; static std::atomic<bool> viewRun{false};
static void viewer(int periodMs) {
  while (viewRun) {
#ifdef NEWAPI
    CamFrame f; if (camFrameAcquire(&f)) { ++viewerGot; camFrameRelease(&f); }
#else
    camera_fb_t* fb = esp_camera_fb_get(); if (fb) { ++viewerGot; esp_camera_fb_return(fb); }
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(periodMs));
  }
}

int main(int argc, char** argv) {
  const char* out = argv[1]; int secs = atoi(argv[2]); int ceiling = atoi(argv[3]);
  g_sdStallEvery = atoi(argv[4]); g_sdStallMs = atoi(argv[5]); int viewers = atoi(argv[6]);
  for (int i = 1; i <= 39; ++i) {
    char p[64]; snprintf(p, sizeof p, "frames/f%02d.jpg", i);
    std::ifstream f(p, std::ios::binary); std::vector<uint8_t> v((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); jpgs.push_back(std::move(v));
  }
  char lg[96]; snprintf(lg, sizeof lg, "%s.produced.txt", out); prodLog = fopen(lg, "w");
  rec_set_config((uint8_t)ceiling, 0, 5, 3);
  if (argc > 7) rec_set_rotation((uint8_t)atoi(argv[7]));          // optional: quarter turns clockwise
  prodRun = true; std::thread pt(producer);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  if (!rec_start(out, true)) { printf("rec_start FAILED\n"); return 1; }
  uint32_t t0 = millis();
  viewRun = true; std::vector<std::thread> vt;
  for (int i = 0; i < viewers; ++i) vt.emplace_back(viewer, 33);
  std::this_thread::sleep_for(std::chrono::seconds(secs));
  uint32_t wall = millis() - t0;
  String r = rec_stop();
  viewRun = false; for (auto& t : vt) t.join();
  prodRun = false; pt.join(); fclose(prodLog);
  printf("RESULT  wall=%ums  sensor produced=%d  viewers got=%d\n        recorder says: %s\n",
         wall, producedN.load(), viewerGot.load(), r.c_str());
  return 0;
}
