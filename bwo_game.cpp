/* BWO Game v2 touch port.
 *
 * The original Rust program owns a standalone ESP32 display/IMU runtime. This
 * module keeps its DDA 3D view and flow vocabulary, but uses the WUW panel's
 * single resistive touch point and a bounded half-resolution framebuffer. */
#include "bwo_game.h"
#include "game_art.h"
#include "link.h"

#include <math.h>
#include <string.h>
#include <Preferences.h>
#include <SD_MMC.h>
#include "esp_heap_caps.h"

namespace {

constexpr int MAP_W = 24;
constexpr int MAP_H = 24;
constexpr int LEVEL_CELLS = MAP_W * MAP_H;
constexpr int WORLD_CELLS = BWO_LEVELS * LEVEL_CELLS;
constexpr int MAX_ENTITIES = 14;
constexpr int WALL_TEX_W = 32;
constexpr int WALL_TEX_H = 32;
constexpr int WALL_TEX_PIXELS = WALL_TEX_W * WALL_TEX_H;
constexpr float PI_F = 3.14159265358979323846f;
constexpr float FOV = 1.16f;                 // about 66 degrees
constexpr float MAX_VIEW = 22.0f;

struct Entity {
  float x;
  float y;
  uint8_t kind;                               // 0 crystal, 1 hostile organ
  uint8_t hp;
  bool active;
  uint8_t level;
};

struct Game {
  float x = 3.5f;
  float y = 3.5f;
  float angle = 0.0f;
  float move = 0.0f;
  float turn = 0.0f;
  int touchX = 0;
  int touchY = 0;
  int anchorX = 0;
  int anchorY = 0;
  bool touching = false;
  bool paused = false;
  bool building = false;
  bool over = false;
  uint8_t intensity = 92;
  uint8_t strata = 5;
  uint8_t resonance = 55;
  uint8_t dt = 0;
  uint8_t memories = 0;
  uint8_t ammo = 24;
  uint8_t muzzle = 0;
  uint8_t selectedWall = 0;
  uint8_t level = 0;
  uint8_t theme = 0;
  uint8_t uv = 0;
  uint8_t glitch = 0;
  BwoFace selectedFace = BWO_FACE_NORTH;
  uint32_t ticks = 0;
  uint32_t lastTick = 0;
};

static Game g;
static uint8_t mapData[WORLD_CELLS];
static uint8_t builtWalls[WORLD_CELLS];
// Six 3-bit material codes per cell: 0 is original color, 1..4 index photos.
static uint32_t faceMaterials[WORLD_CELLS];
static bool worldDirty = false;
static uint32_t worldDirtyAt = 0;
static uint32_t worldSaveDelay = 1500;
static bool worldSdReady = false;
static uint32_t worldGeneration = 0;
static Entity entities[MAX_ENTITIES];
static uint16_t* frameBuf = nullptr;
static uint16_t* wallTextures = nullptr;
static uint8_t wallTextureCount = 0;
static uint8_t wallTextureNext = 0;
static float zBuf[BWO_FB_W];

// Each named slot has two independently verified generations. Texture pixels
// travel with the patch, so gallery-slot changes cannot rewrite older worlds.
struct PatchData {
  uint8_t walls[WORLD_CELLS];
  uint32_t faces[WORLD_CELLS];
  uint16_t photos[BWO_WALL_TEXTURE_SLOTS * WALL_TEX_PIXELS];
  float x, y, angle;
  uint16_t inactive;
  uint8_t level, theme, uv, glitch, memories, ammo, intensity;
  uint8_t photoCount, selectedPhoto, nextPhoto;
};

struct WorldHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t generation;
  uint32_t length;
  uint32_t checksum;
};
constexpr uint32_t WORLD_MAGIC = 0x334f5742u;  // BWO3
constexpr uint32_t WORLD_VERSION = 1;
constexpr size_t WORLD_BYTES = sizeof(builtWalls) + sizeof(faceMaterials);
static const char* WORLD_FILES[] = { "/BWO_WORLD_A.bin", "/BWO_WORLD_B.bin" };

static uint32_t checksumBytes(uint32_t sum, const uint8_t* bytes, size_t len) {
  for (size_t i = 0; i < len; ++i) sum = (sum ^ bytes[i]) * 16777619u;
  return sum;
}

static void patchPath(char* path, size_t size, uint8_t slot, int bank) {
  snprintf(path, size, "/BWO_PATCH_%u_%c.bin", (unsigned)slot + 1, bank ? 'B' : 'A');
}

static uint32_t inspectPatch(uint8_t slot, int bank) {
  char path[32]; patchPath(path, sizeof(path), slot, bank);
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) return 0;
  WorldHeader h = {};
  bool ok = f.size() == sizeof(h) + sizeof(PatchData) &&
    f.readBytes((char*)&h, sizeof(h)) == sizeof(h) && h.magic == 0x50555742u &&
    h.version == 1 && h.length == sizeof(PatchData) && h.generation != 0;
  uint8_t buf[512]; size_t left = sizeof(PatchData); uint32_t sum = 2166136261u;
  while (ok && left) {
    size_t n = min(left, sizeof(buf));
    if (f.read(buf, n) != n) { ok = false; break; }
    sum = checksumBytes(sum, buf, n); left -= n;
  }
  f.close();
  return ok && sum == h.checksum ? h.generation : 0;
}

static bool validPatch(const PatchData& p) {
  if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.angle) || p.x < 1 || p.y < 1 ||
      p.x >= MAP_W-1 || p.y >= MAP_H-1 || fabsf(p.angle) > 1000 ||
      p.level >= BWO_LEVELS || p.theme > 3 || p.uv > 3 || p.glitch > 3 ||
      p.memories > 7 || p.intensity > 100 || p.photoCount > BWO_WALL_TEXTURE_SLOTS ||
      p.selectedPhoto >= BWO_WALL_TEXTURE_SLOTS || p.nextPhoto >= BWO_WALL_TEXTURE_SLOTS ||
      (p.inactive >> MAX_ENTITIES)) return false;
  for (int i = 0; i < WORLD_CELLS; ++i) {
    uint8_t w = p.walls[i];
    if (w && w != 0x40 && (w < 0x80 || w > 0x84)) return false;
    int x = i % MAP_W, y = (i / MAP_W) % MAP_H;
    if (w && (x == 0 || y == 0 || x == MAP_W-1 || y == MAP_H-1)) return false;
    if (p.faces[i] >> 18) return false;
    for (int face = 0; face < 6; ++face)
      if (((p.faces[i] >> (face * 3)) & 7) > 4) return false;
  }
  int collected=0;
  for(int i=0;i<7;++i)if(p.inactive&(1u<<i))++collected;
  if(collected!=p.memories) return false;
  for(int corner=0;corner<4;++corner) {
    int x=(int)(p.x+(corner&1?.18f:-.18f));
    int y=(int)(p.y+(corner&2?.18f:-.18f));
    uint8_t override=p.walls[p.level*LEVEL_CELLS+y*MAP_W+x];
    bool wall=(x%6==0&&y%6!=2&&y%6!=3)||(y%6==0&&x%6!=2&&x%6!=3)||
      (x>2&&y>2&&(x*11+y*7+p.level*13)%43==0);
    if((override&0x80)||(override!=0x40&&wall))return false;
  }
  return true;
}

static uint32_t worldChecksum() {
  uint32_t sum = checksumBytes(2166136261u, builtWalls, sizeof(builtWalls));
  return checksumBytes(sum, (const uint8_t*)faceMaterials,
                       sizeof(faceMaterials));
}

static bool inspectWorldSlot(int slot, uint32_t* generation) {
  File f = SD_MMC.open(WORLD_FILES[slot], FILE_READ);
  if (!f || f.isDirectory()) { if (f) f.close(); return false; }
  WorldHeader h = {};
  if (f.size() != sizeof(h) + WORLD_BYTES ||
      f.readBytes((char*)&h, sizeof(h)) != sizeof(h) ||
      h.magic != WORLD_MAGIC || h.version != WORLD_VERSION ||
      h.length != WORLD_BYTES) { f.close(); return false; }
  uint8_t chunk[512];
  size_t left = WORLD_BYTES;
  uint32_t sum = 2166136261u;
  while (left) {
    size_t n = min(left, sizeof(chunk));
    if (f.read(chunk, n) != n) { f.close(); return false; }
    sum = checksumBytes(sum, chunk, n);
    left -= n;
  }
  f.close();
  if (sum != h.checksum) return false;
  *generation = h.generation;
  return true;
}

static bool loadWorldSlot(int slot) {
  File f = SD_MMC.open(WORLD_FILES[slot], FILE_READ);
  if (!f || !f.seek(sizeof(WorldHeader))) { if (f) f.close(); return false; }
  bool ok = f.readBytes((char*)builtWalls, sizeof(builtWalls)) ==
              sizeof(builtWalls) &&
            f.readBytes((char*)faceMaterials, sizeof(faceMaterials)) ==
              sizeof(faceMaterials);
  f.close();
  return ok;
}

static bool saveWorldSlot() {
  if (!worldSdReady) return false;
  uint32_t next = worldGeneration + 1;
  int slot = next & 1;
  File f = SD_MMC.open(WORLD_FILES[slot], "w");
  if (!f) return false;
  WorldHeader h = { WORLD_MAGIC, WORLD_VERSION, next, WORLD_BYTES,
                    worldChecksum() };
  bool ok = f.write((const uint8_t*)&h, sizeof(h)) == sizeof(h) &&
            f.write(builtWalls, sizeof(builtWalls)) == sizeof(builtWalls) &&
            f.write((const uint8_t*)faceMaterials, sizeof(faceMaterials)) ==
              sizeof(faceMaterials);
  f.flush(); f.close();
  uint32_t checked = 0;
  if (!ok || !inspectWorldSlot(slot, &checked) || checked != next) return false;
  worldGeneration = next;
  return true;
}

static int cellIndex(int level, int x, int y) {
  return level * LEVEL_CELLS + y * MAP_W + x;
}

static uint8_t faceMaterial(int level, int x, int y, BwoFace face) {
  if (level < 0 || level >= BWO_LEVELS || x < 0 || y < 0 ||
      x >= MAP_W || y >= MAP_H) return 0;
  return (faceMaterials[cellIndex(level, x, y)] >> (face * 3)) & 7;
}

static void setFaceMaterial(int level, int x, int y, BwoFace face,
                            uint8_t material) {
  int i = cellIndex(level, x, y);
  uint32_t shift = face * 3;
  faceMaterials[i] = (faceMaterials[i] & ~(7u << shift)) |
                     ((uint32_t)(material & 7) << shift);
  worldDirty = true; worldDirtyAt = millis();
}

static uint16_t color565(uint8_t r, uint8_t green, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((green & 0xFC) << 3) | (b >> 3));
}

static uint16_t shade(uint16_t c, float amount) {
  if (amount < 0.04f) amount = 0.04f;
  if (amount > 1.0f) amount = 1.0f;
  uint8_t r = (uint8_t)(((c >> 11) & 31) * amount);
  uint8_t green = (uint8_t)(((c >> 5) & 63) * amount);
  uint8_t b = (uint8_t)((c & 31) * amount);
  return (uint16_t)((r << 11) | (green << 5) | b);
}

static uint16_t shadeFixed(uint16_t c, uint16_t scale) {
  return (uint16_t)(((((c >> 11) & 31) * scale) >> 8) << 11 |
                    ((((c >> 5) & 63) * scale) >> 8) << 5 |
                    (((c & 31) * scale) >> 8));
}

static uint8_t tileAt(int x, int y) {
  if (x < 0 || y < 0 || x >= MAP_W || y >= MAP_H) return 1;
  return mapData[cellIndex(g.level, x, y)];
}

static uint8_t roomAt(int x, int y) {
  int rx = x / 6;
  int ry = y / 6;
  return (uint8_t)((rx * 3 + ry * 5 + rx * ry) % 7);
}

static uint16_t roomColor(uint8_t room) {
  static const uint16_t colors[] = {
    color565(24, 225, 230), color565(236, 26, 85),
    color565(94, 230, 116), color565(220, 205, 44),
    color565(196, 58, 230), color565(55, 92, 235),
    color565(132, 58, 207),
  };
  return colors[room % 7];
}

static bool openAt(float x, float y) {
  const float radius = 0.18f;
  return tileAt((int)(x - radius), (int)(y - radius)) == 0 &&
         tileAt((int)(x + radius), (int)(y - radius)) == 0 &&
         tileAt((int)(x - radius), (int)(y + radius)) == 0 &&
         tileAt((int)(x + radius), (int)(y + radius)) == 0;
}

static void makeWorld() {
  memset(mapData, 0, sizeof(mapData));
  for (int level = 0; level < BWO_LEVELS; ++level) {
    for (int y = 0; y < MAP_H; ++y) {
      for (int x = 0; x < MAP_W; ++x) {
      bool wall = x == 0 || y == 0 || x == MAP_W - 1 || y == MAP_H - 1;
      if (!wall && x % 6 == 0 && (y % 6 != 2 && y % 6 != 3)) wall = true;
      if (!wall && y % 6 == 0 && (x % 6 != 2 && x % 6 != 3)) wall = true;
      if (!wall && x > 2 && y > 2 &&
          ((x * 11 + y * 7 + level * 13) % 43 == 0)) wall = true;
      mapData[cellIndex(level, x, y)] = wall ?
        (uint8_t)(roomAt(x, y) + 1) : 0;
      }
    }
  }
  for (int i = 0; i < WORLD_CELLS; ++i)
    if (builtWalls[i] == 0x40) mapData[i] = 0;
    else if (builtWalls[i] & 0x80) mapData[i] = builtWalls[i];

  static const float points[MAX_ENTITIES][2] = {
    {4.5f, 3.5f}, {8.5f, 2.5f}, {14.5f, 3.5f}, {20.5f, 8.5f},
    {15.5f, 14.5f}, {8.5f, 20.5f}, {3.5f, 15.5f},
    {9.5f, 9.5f}, {15.5f, 8.5f}, {20.5f, 14.5f}, {14.5f, 20.5f},
    {3.5f, 20.5f}, {20.5f, 20.5f}, {9.5f, 15.5f},
  };
  for (int i = 0; i < MAX_ENTITIES; ++i) {
    entities[i] = { points[i][0], points[i][1], (uint8_t)(i < 7 ? 0 : 1),
                    (uint8_t)(i < 7 ? 1 : 3), true, (uint8_t)(i % BWO_LEVELS) };
  }
}

static float wrapAngle(float a) {
  while (a > PI_F) a -= PI_F * 2.0f;
  while (a < -PI_F) a += PI_F * 2.0f;
  return a;
}

static bool clearSight(float x0, float y0, float x1, float y1) {
  float dx = x1 - x0;
  float dy = y1 - y0;
  float distance = sqrtf(dx * dx + dy * dy);
  int steps = (int)(distance * 8.0f);
  if (steps < 1) return true;
  for (int i = 1; i < steps; ++i) {
    float t = (float)i / (float)steps;
    if (tileAt((int)(x0 + dx * t), (int)(y0 + dy * t))) return false;
  }
  return true;
}

static bool tryPhotoPortal() {
  int sourceX = -1, sourceY = -1, sourceLevel = g.level;
  uint8_t portal = 0;
  for (float d = 0.25f; d <= 2.5f; d += 0.10f) {
    int x = (int)(g.x + cosf(g.angle) * d);
    int y = (int)(g.y + sinf(g.angle) * d);
    uint8_t tile = tileAt(x, y);
    if (!tile) continue;
    if (!(tile & 0x80)) return false;
    sourceX = x; sourceY = y; portal = tile;
    break;
  }
  if (!(portal & 0x0f)) return false;

  // A photo becomes a portal once the same texture is placed on a second
  // wall. Land beside the paired wall, never inside geometry or an entity.
  static const int8_t nx[] = { 1, -1, 0, 0 };
  static const int8_t ny[] = { 0, 0, 1, -1 };
  for (int i = 0; i < WORLD_CELLS; ++i) {
    int tx = i % MAP_W, ty = (i / MAP_W) % MAP_H, level = i / LEVEL_CELLS;
    if (builtWalls[i] != portal ||
        (tx == sourceX && ty == sourceY && level == sourceLevel)) continue;
    for (int n = 0; n < 4; ++n) {
      float px = tx + nx[n] + 0.5f;
      float py = ty + ny[n] + 0.5f;
      uint8_t oldLevel = g.level;
      g.level = level;
      bool clear = openAt(px, py);
      g.level = oldLevel;
      if (!clear) continue;
      g.level = level;
      g.x = px; g.y = py;
      g.angle = atan2f(py - ((float)ty + 0.5f),
                       px - ((float)tx + 0.5f));
      g.muzzle = 8;
      g.dt = (uint8_t)min(100, (int)g.dt + 10);
      return true;
    }
  }
  return false;
}

static void fireWeapon() {
  if (g.paused || g.over) return;
  if (tryPhotoPortal()) return;
  if (g.ammo == 0) return;
  --g.ammo;
  g.muzzle = 5;
  int target = -1;
  float best = MAX_VIEW;
  for (int i = 0; i < MAX_ENTITIES; ++i) {
    Entity& e = entities[i];
    if (!e.active || e.level != g.level || e.kind != 1) continue;
    float dx = e.x - g.x;
    float dy = e.y - g.y;
    float distance = sqrtf(dx * dx + dy * dy);
    float delta = fabsf(wrapAngle(atan2f(dy, dx) - g.angle));
    if (delta < 0.11f && distance < best && clearSight(g.x, g.y, e.x, e.y)) {
      target = i;
      best = distance;
    }
  }
  if (target >= 0 && --entities[target].hp == 0) {
    entities[target].active = false;
    g.dt = (uint8_t)min(100, (int)g.dt + 14);
    g.intensity = (uint8_t)min(100, (int)g.intensity + 5);
  }
}

static void updateGame(float dt) {
  if (g.paused || g.over) return;
  g.angle = wrapAngle(g.angle + g.turn * dt * 2.2f);
  float step = g.move * dt * (1.55f - g.strata * 0.006f);
  float nx = g.x + cosf(g.angle) * step;
  float ny = g.y + sinf(g.angle) * step;
  if (openAt(nx, g.y)) g.x = nx;
  if (openAt(g.x, ny)) g.y = ny;

  for (int i = 0; i < MAX_ENTITIES; ++i) {
    Entity& e = entities[i];
    if (!e.active || e.level != g.level) continue;
    float dx = e.x - g.x;
    float dy = e.y - g.y;
    float d2 = dx * dx + dy * dy;
    if (e.kind == 0 && d2 < 0.38f) {
      e.active = false;
      ++g.memories;
      g.resonance = (uint8_t)min(100, (int)g.resonance + 15);
      g.intensity = (uint8_t)min(100, (int)g.intensity + 12);
      g.dt = (uint8_t)min(100, (int)g.dt + 18);
    } else if (e.kind == 1 && d2 < 35.0f) {
      float distance = sqrtf(d2);
      if (distance > 0.65f) {
        float speed = 0.23f * dt / distance;
        float ex = e.x - dx * speed;
        float ey = e.y - dy * speed;
        if (openAt(ex, e.y)) e.x = ex;
        if (openAt(e.x, ey)) e.y = ey;
      } else if ((g.ticks % 24) == 0) {
        g.intensity = g.intensity > 4 ? g.intensity - 4 : 0;
        g.strata = (uint8_t)min(100, (int)g.strata + 5);
        if (!g.intensity) g.over = true;
      }
    }
  }

  if ((g.ticks % 90) == 0) {
    if (g.strata) --g.strata;
    if (g.move != 0.0f) g.dt = (uint8_t)min(100, (int)g.dt + 1);
    if (g.ammo < 24 && g.move == 0.0f) ++g.ammo;
  }
  if (g.dt >= 100) {
    g.dt = 0;
    g.strata = 0;
    g.resonance = 100;
    g.intensity = (uint8_t)min(100, (int)g.intensity + 25);
  }
  if (g.muzzle) --g.muzzle;
}

static void putPixel(int x, int y, uint16_t c) {
  if ((unsigned)x < BWO_FB_W && (unsigned)y < BWO_FB_H)
    frameBuf[y * BWO_FB_W + x] = c;
}

static void fillRect(int x, int y, int w, int h, uint16_t c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > BWO_FB_W) w = BWO_FB_W - x;
  if (y + h > BWO_FB_H) h = BWO_FB_H - y;
  if (w <= 0 || h <= 0) return;
  for (int yy = y; yy < y + h; ++yy)
    for (int xx = x; xx < x + w; ++xx) frameBuf[yy * BWO_FB_W + xx] = c;
}

static void rotateUv(int& x, int& y) {
  for (uint8_t turn=0;turn<g.uv;++turn) { int old=x; x=31-y; y=old; }
}

static void renderWorld() {
  const int artBase = g.theme == 3 ? 32 : g.theme ? 16 : 0;
  uint8_t room = roomAt((int)g.x, (int)g.y) + g.level * 2;
  float dirX = cosf(g.angle);
  float dirY = sinf(g.angle);
  float planeX = -dirY * tanf(FOV * 0.5f);
  float planeY = dirX * tanf(FOV * 0.5f);
  {
    // Fixed atlas tiles need no decode or heap allocation during play.
    for (int y = 0; y < BWO_FB_H; ++y) {
      float dy = fabsf((float)y + 0.5f - BWO_FB_H * 0.5f);
      float dist = min(MAX_VIEW, (BWO_FB_H * 0.5f) / dy);
      float wx = g.x + dist * (dirX - planeX);
      float wy = g.y + dist * (dirY - planeY);
      float dx = 2.0f * dist * planeX / BWO_FB_W;
      float stepY = 2.0f * dist * planeY / BWO_FB_W;
      BwoFace face = y < BWO_FB_H / 2 ? BWO_FACE_CEILING : BWO_FACE_FLOOR;
      float light = min(1.0f, max(face == BWO_FACE_FLOOR ? 0.22f : 0.34f,
                                 1.45f / (1.0f + dist * dist * 0.07f)));
      uint16_t scale = (uint16_t)(light * 256.0f);
      for (int x = 0; x < BWO_FB_W; ++x, wx += dx, wy += stepY) {
        int cellX = (int)floorf(wx), cellY = (int)floorf(wy);
        uint8_t mat = faceMaterial(g.level, cellX, cellY, face);
        if (!mat && cellX >= 0 && cellY >= 0 &&
            cellX < MAP_W && cellY < MAP_H) {
          int adjacent = face == BWO_FACE_FLOOR ? (int)g.level - 1 :
                         (int)g.level + 1;
          BwoFace blockFace = face == BWO_FACE_FLOOR ?
            BWO_FACE_CEILING : BWO_FACE_FLOOR;
          if (adjacent >= 0 && adjacent < BWO_LEVELS &&
              (builtWalls[cellIndex(adjacent, cellX, cellY)] & 0x80))
            mat = faceMaterial(adjacent, cellX, cellY, blockFace);
        }
        int texX = constrain((int)((wx - cellX) * WALL_TEX_W),
                             0, WALL_TEX_W - 1);
        int texY = constrain((int)((wy - cellY) * WALL_TEX_H),
                             0, WALL_TEX_H - 1);
        rotateUv(texX, texY);
        uint16_t pixel = mat && wallTextures && wallTextureCount
          ? wallTextures[((mat - 1) % wallTextureCount) *
                         WALL_TEX_PIXELS + texY * WALL_TEX_W + texX]
          : WuwGameArt::sample(artBase + (face == BWO_FACE_FLOOR ? 12 + (g.level == 1 ? 2 : 0) : 13),
                               texX, texY);
        putPixel(x, y, shadeFixed(pixel, scale));
      }
    }
  }
  for (int x = 0; x < BWO_FB_W; ++x) {
    float cameraX = 2.0f * x / (float)BWO_FB_W - 1.0f;
    float rayX = dirX + planeX * cameraX;
    float rayY = dirY + planeY * cameraX;
    int mapX = (int)g.x;
    int mapY = (int)g.y;
    float deltaX = fabsf(rayX) < 0.0001f ? 1e9f : fabsf(1.0f / rayX);
    float deltaY = fabsf(rayY) < 0.0001f ? 1e9f : fabsf(1.0f / rayY);
    int stepX = rayX < 0 ? -1 : 1;
    int stepY = rayY < 0 ? -1 : 1;
    float sideX = rayX < 0 ? (g.x - mapX) * deltaX : (mapX + 1.0f - g.x) * deltaX;
    float sideY = rayY < 0 ? (g.y - mapY) * deltaY : (mapY + 1.0f - g.y) * deltaY;
    int side = 0;
    uint8_t hit = 0;
    for (int depth = 0; depth < 32 && !hit; ++depth) {
      if (sideX < sideY) { sideX += deltaX; mapX += stepX; side = 0; }
      else               { sideY += deltaY; mapY += stepY; side = 1; }
      hit = tileAt(mapX, mapY);
    }
    float distance = side == 0 ? sideX - deltaX : sideY - deltaY;
    if (distance < 0.08f) distance = 0.08f;
    if (distance > MAX_VIEW) distance = MAX_VIEW;
    zBuf[x] = distance;
    int wallH = (int)(BWO_FB_H / distance);
    int y0 = max(0, (BWO_FB_H - wallH) / 2);
    int y1 = min(BWO_FB_H - 1, (BWO_FB_H + wallH) / 2);
    float light = 1.0f / (1.0f + distance * distance * 0.045f);
    if (side) light *= 0.68f;
    float wallX = side == 0 ? g.y + distance * rayY : g.x + distance * rayX;
    wallX -= floorf(wallX);
    int texX = constrain((int)(wallX * WALL_TEX_W), 0, WALL_TEX_W - 1);
    if ((side == 0 && rayX > 0) || (side == 1 && rayY < 0))
      texX = WALL_TEX_W - 1 - texX;
    int textureSlot = -1;
    if (wallTextures && wallTextureCount) {
      BwoFace face = side == 0 ?
        (stepX > 0 ? BWO_FACE_WEST : BWO_FACE_EAST) :
        (stepY > 0 ? BWO_FACE_NORTH : BWO_FACE_SOUTH);
      uint8_t mat = faceMaterial(g.level, mapX, mapY, face);
      if (mat)
        textureSlot = (mat - 1) % wallTextureCount;
      else if ((hit & 0x80) && (hit & 0x0f))
        textureSlot = ((hit & 0x0f) - 1) % wallTextureCount;
    }
    for (int y = y0; y <= y1; ++y) {
      // Use the unclipped wall height to avoid stretching photos up close.
      int texY = constrain((y - (BWO_FB_H - wallH) / 2) * WALL_TEX_H /
                           max(1, wallH), 0, WALL_TEX_H - 1);
      int u=texX,v=texY; rotateUv(u,v);
      if (textureSlot >= 0) {
        uint16_t photo = wallTextures[textureSlot * WALL_TEX_PIXELS +
                                      v * WALL_TEX_W + u];
        // Lift distant photos enough to remain legible, while preserving the
        // DDA depth cue and a dark seam around each repeated image.
        float photoLight = min(1.0f, max(0.18f, light * 1.65f));
        if (texX == 0 || texY == 0 || texX == WALL_TEX_W - 1 ||
            texY == WALL_TEX_H - 1) photoLight *= 0.35f;
        putPixel(x, y, shade(photo, photoLight));
      } else {
        uint16_t pixel = WuwGameArt::sample(artBase + 8 + ((mapX / 6 + mapY / 6 + g.level + (g.theme == 2 ? 1 : 0)) & 3), u, v);
        putPixel(x, y, shadeFixed(pixel, (uint16_t)(max(0.24f, light) * 256)));
      }
    }
  }

  // Billboard crystals and hostiles, drawn far-to-near and depth tested.
  bool drawn[MAX_ENTITIES] = {};
  for (int pass = 0; pass < MAX_ENTITIES; ++pass) {
    int pick = -1;
    float farthest = -1.0f;
    for (int i = 0; i < MAX_ENTITIES; ++i) {
      if (!entities[i].active || entities[i].level != g.level || drawn[i]) continue;
      float dx = entities[i].x - g.x;
      float dy = entities[i].y - g.y;
      float d2 = dx * dx + dy * dy;
      if (d2 > farthest) { farthest = d2; pick = i; }
    }
    if (pick < 0) break;
    drawn[pick] = true;
    Entity& e = entities[pick];
    float dx = e.x - g.x;
    float dy = e.y - g.y;
    float distance = sqrtf(dx * dx + dy * dy);
    float depth = dx * dirX + dy * dirY;
    if (depth < 0.2f || distance > MAX_VIEW) continue;
    float lateral = -dx * dirY + dy * dirX;
    int sx = (int)(BWO_FB_W * 0.5f * (1.0f + lateral / (depth * tanf(FOV * 0.5f))));
    int size = constrain((int)((e.kind ? 74.0f : 44.0f) / depth), 3, 76);
    int top = BWO_FB_H / 2 + (int)(BWO_FB_H * 0.5f / depth) - size;
    int sprite = e.kind ? 2 + ((g.ticks / 9 + pick) & 1) : (artBase ? artBase + pick % 8 : 32 + pick % 8);
    for (int yy = 0; yy < size; ++yy)
      for (int xx = 0; xx < size; ++xx) {
        int px = sx - size / 2 + xx;
        if ((unsigned)px >= BWO_FB_W || depth >= zBuf[px]) continue;
        uint16_t pixel = WuwGameArt::sample(sprite, xx * 32 / size, yy * 32 / size);
        if (pixel) putPixel(px, top + yy,
                           shadeFixed(pixel, (uint16_t)(max(0.4f, 1.0f-distance/32) * 256)));
      }
  }

  // Nearby WUW cameras are depth-tested billboards. Only positions cross the
  // radio link; each panel keeps rendering its own world at full frame rate.
  LinkGamePeer visitors[LINK_MAX_PEERS];
  size_t visitorCount = linkGamePeers(visitors, LINK_MAX_PEERS);
  for (size_t i = 0; i < visitorCount; ++i) {
    const LinkGamePose& p = visitors[i].pose;
    if (p.level != g.level) continue;
    float dx = p.x100 * 0.01f - g.x;
    float dy = p.y100 * 0.01f - g.y;
    float depth = dx * dirX + dy * dirY;
    if (depth < 0.25f || depth > MAX_VIEW) continue;
    float lateral = -dx * dirY + dy * dirX;
    int sx = (int)(BWO_FB_W * 0.5f *
                   (1.0f + lateral / (depth * tanf(FOV * 0.5f))));
    int size = constrain((int)(58.0f / depth), 4, 64);
    int top = BWO_FB_H / 2 + (int)(BWO_FB_H * 0.5f / depth) - size;
    uint16_t edge = (visitors[i].id[7] & 1)
      ? color565(245, 91, 174) : color565(69, 235, 222);
    for (int yy = 0; yy < size; ++yy) {
      for (int xx = 0; xx < size; ++xx) {
        int px = sx - size / 2 + xx;
        if ((unsigned)px >= BWO_FB_W || depth >= zBuf[px]) continue;
        uint16_t pixel = WuwGameArt::sample(7, xx * 32 / size,
                                              yy * 32 / size);
        if (pixel) putPixel(px, top + yy, pixel);
        else if (xx == 0 || xx == size - 1 || yy == 0 || yy == size - 1)
          putPixel(px, top + yy, edge);
      }
    }
  }

  // A bounded scan displacement uses one stack row, never another framebuffer.
  if (g.glitch) {
    uint16_t row[BWO_FB_W];
    int start=(g.ticks / 5 * 7) % BWO_FB_H;
    for(int band=0;band<g.glitch;++band) {
      int y=(start+band*19)%BWO_FB_H;
      memcpy(row,frameBuf+y*BWO_FB_W,sizeof(row));
      for(int x=0;x<BWO_FB_W;++x)frameBuf[y*BWO_FB_W+x]=row[(x+g.glitch*2)%BWO_FB_W];
    }
  }
  uint16_t reticle = g.muzzle ? color565(255, 245, 205) : color565(80, 245, 225);
  putPixel(BWO_FB_W / 2 - 3, BWO_FB_H / 2, reticle);
  putPixel(BWO_FB_W / 2 + 3, BWO_FB_H / 2, reticle);
  putPixel(BWO_FB_W / 2, BWO_FB_H / 2 - 3, reticle);
  putPixel(BWO_FB_W / 2, BWO_FB_H / 2 + 3, reticle);

  // Compact flow HUD. Bars remain readable after the panel's 2x expansion.
  fillRect(3, 14, 72, 17, color565(23, 30, 31));
  for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x) {
    uint16_t pixel = WuwGameArt::sample(7, x * 2, y * 2);
    if (pixel) putPixel(4 + x, 14 + y, pixel);
  }
  fillRect(22, 17, 51, 4, color565(191, 184, 151));
  fillRect(23, 18, g.intensity / 2, 2, color565(196, 69, 83));
  fillRect(22, 24, 51, 3, color565(191, 184, 151));
  fillRect(23, 25, g.resonance / 2, 1, color565(74, 203, 175));
  fillRect(BWO_FB_W - 31, 3, 28, 4, color565(5, 12, 15));
  fillRect(BWO_FB_W - 30, 4, g.dt * 26 / 100, 2, color565(214, 78, 244));
  if (g.building) {
    uint16_t selected = wallTextures && wallTextureCount
      ? wallTextures[(g.selectedWall % wallTextureCount) * WALL_TEX_PIXELS +
                     WALL_TEX_PIXELS / 2]
      : color565(255, 205, 60);
    fillRect(BWO_FB_W - 11, 9, 8, 8, selected);
    fillRect(BWO_FB_W - 12, 8, 10, 1, color565(255, 245, 205));
    fillRect(BWO_FB_W - 12, 17, 10, 1, color565(255, 245, 205));
  }
  // Three illuminated ticks make the vertical position visible at a glance.
  for (int level = 0; level < BWO_LEVELS; ++level)
    fillRect(60 + level * 6, 3, 4, 3,
             level == g.level ? color565(255, 245, 205) :
             color565(37, 74, 77));
  for (size_t i = 0; i < visitorCount; ++i)
    fillRect(92 + i * 6, 3, 4, 3, color565(69, 235, 222));

  // Screen-edge arrows communicate the resistive one-finger drag control.
  uint16_t hint = shade(roomColor(room), 0.55f);
  for (int i = 0; i < 5; ++i) {
    putPixel(3 + i, BWO_FB_H / 2 - i, hint);
    putPixel(3 + i, BWO_FB_H / 2 + i, hint);
    putPixel(BWO_FB_W - 4 - i, BWO_FB_H / 2 - i, hint);
    putPixel(BWO_FB_W - 4 - i, BWO_FB_H / 2 + i, hint);
  }

  if (g.paused || g.over) {
    uint16_t panel = color565(4, 8, 12);
    fillRect(43, 31, 74, 22, panel);
    uint16_t edge = g.over ? color565(242, 34, 88) : color565(60, 255, 224);
    fillRect(43, 31, 74, 1, edge);
    fillRect(43, 52, 74, 1, edge);
    // Pause mark or dissolution X, avoiding a font dependency in the engine.
    if (g.over) {
      for (int i = 0; i < 12; ++i) {
        putPixel(74 + i, 36 + i, edge);
        putPixel(85 - i, 36 + i, edge);
      }
    } else {
      fillRect(75, 36, 3, 12, edge);
      fillRect(82, 36, 3, 12, edge);
    }
  }
}

}  // namespace

bool bwoGameBegin(bool sdAvailable) {
  worldSdReady = sdAvailable;
  if (!frameBuf) {
    frameBuf = static_cast<uint16_t*>(heap_caps_malloc(
      (size_t)BWO_FB_W * BWO_FB_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM));
    if (!frameBuf) {
      frameBuf = static_cast<uint16_t*>(heap_caps_malloc(
        (size_t)BWO_FB_W * BWO_FB_H * sizeof(uint16_t), MALLOC_CAP_INTERNAL));
    }
  }
  if (!frameBuf) return false;
  static bool initialized = false;
  if (!initialized) {
    bool fromSd = false;
    if (worldSdReady) {
      uint32_t a = 0, b = 0;
      bool hasA = inspectWorldSlot(0, &a);
      bool hasB = inspectWorldSlot(1, &b);
      int first = hasB && (!hasA || b > a) ? 1 : 0;
      if ((first == 0 && hasA) || (first == 1 && hasB)) {
        fromSd = loadWorldSlot(first);
        if (fromSd) worldGeneration = first ? b : a;
      }
      if (!fromSd && ((first == 0 && hasB) || (first == 1 && hasA))) {
        fromSd = loadWorldSlot(1 - first);
        if (fromSd) worldGeneration = first ? a : b;
      }
    }
    if (!fromSd) {
      memset(builtWalls, 0, sizeof(builtWalls));
      memset(faceMaterials, 0, sizeof(faceMaterials));
      Preferences wp; wp.begin("bwoworld", true);
      bool imported = false;
      if (wp.getBytesLength("walls3") == sizeof(builtWalls)) {
        wp.getBytes("walls3", builtWalls, sizeof(builtWalls));
        if (wp.getBytesLength("faces3") == sizeof(faceMaterials))
          wp.getBytes("faces3", faceMaterials, sizeof(faceMaterials));
        imported = true;
      } else if (wp.getBytesLength("walls") == LEVEL_CELLS) {
        // Older one-floor rooms survive the world upgrade.
        wp.getBytes("walls", builtWalls, LEVEL_CELLS);
        for (int i = 0; i < LEVEL_CELLS; ++i) {
          if (!(builtWalls[i] & 0x80)) continue;
          uint32_t material = (builtWalls[i] & 0x0f);
          for (int face = 0; face < 4; ++face)
            faceMaterials[i] |= material << (face * 3);
        }
        imported = true;
      }
      wp.end();
      if (imported && worldSdReady) {
        worldDirty = true; worldDirtyAt = millis();
      }
    }
    g.level = 0;
    makeWorld();
    initialized = true;
  }
  // The OS may have left this screen for minutes. Resume from now rather than
  // trying to simulate all the time spent in the camera or gallery screens.
  g.lastTick = millis();
  return true;
}

void bwoGameReset() {
  g = Game();
  memset(builtWalls, 0, sizeof(builtWalls));
  memset(faceMaterials, 0, sizeof(faceMaterials));
  Preferences wp; wp.begin("bwoworld", false);
  wp.remove("walls3"); wp.remove("faces3"); wp.remove("walls"); wp.end();
  worldDirty = true;
  worldDirtyAt = millis() - 1501;
  worldSaveDelay = 1500;
  makeWorld();
  if (frameBuf) memset(frameBuf, 0, (size_t)BWO_FB_W * BWO_FB_H * sizeof(uint16_t));
}

static char pilgrimName[BWO_NAME_MAX + 1] = BWO_NAME_DEFAULT;
static bool pilgrimNameLoaded = false;

const char* bwoGameName() {
  if (!pilgrimNameLoaded) {
    pilgrimNameLoaded = true;                       // one read of flash; the name lives in RAM after that
    Preferences wp;
    if (wp.begin("bwoworld", true)) {
      char stored[BWO_NAME_MAX + 1] = "";
      wp.getString("name", stored, sizeof(stored));
      wp.end();
      bwoNameClean(stored, pilgrimName);            // an empty or damaged value becomes the default
    }
  }
  return pilgrimName;
}

bool bwoGameSetName(const char* name) {
  bwoGameName();                                    // make sure the stored name was read first
  char clean[BWO_NAME_MAX + 1];
  size_t len = bwoNameClean(name, clean);
  memcpy(pilgrimName, clean, len + 1);
  Preferences wp;
  if (!wp.begin("bwoworld", false)) return false;
  bool ok = true;
  if (strcmp(clean, BWO_NAME_DEFAULT) == 0) wp.remove("name");     // the default needs no storage
  else ok = wp.putString("name", clean) == len;
  wp.end();
  return ok;
}

bool bwoGamePatchExists(uint8_t slot) {
  if (!worldSdReady || slot >= 4) return false;
  PatchData* p=(PatchData*)heap_caps_malloc(sizeof(PatchData),MALLOC_CAP_SPIRAM);
  if(!p) p=(PatchData*)malloc(sizeof(PatchData));
  if(!p) return false;
  bool found=false;
  for(int bank=0;bank<2&&!found;++bank) {
    if(!inspectPatch(slot,bank)) continue;
    char path[32];patchPath(path,sizeof(path),slot,bank);
    File f=SD_MMC.open(path,FILE_READ);
    WorldHeader h={};
    found=f && f.read((uint8_t*)&h,sizeof(h))==sizeof(h) &&
      f.read((uint8_t*)p,sizeof(PatchData))==sizeof(PatchData) &&
      h.checksum==checksumBytes(2166136261u,(const uint8_t*)p,sizeof(PatchData)) &&
      validPatch(*p);
    if(f) f.close();
  }
  free(p);
  return found;
}

bool bwoGameSavePatch(uint8_t slot) {
  if (!worldSdReady || slot >= 4 || !frameBuf) return false;
  PatchData* p = (PatchData*)heap_caps_calloc(1, sizeof(PatchData), MALLOC_CAP_SPIRAM);
  if (!p) p = (PatchData*)calloc(1, sizeof(PatchData));
  if (!p) return false;
  memcpy(p->walls, builtWalls, sizeof(builtWalls));
  memcpy(p->faces, faceMaterials, sizeof(faceMaterials));
  if (wallTextures && wallTextureCount) memcpy(p->photos, wallTextures, sizeof(p->photos));
  p->x=g.x; p->y=g.y; p->angle=g.angle; p->level=g.level;
  p->theme=g.theme; p->uv=g.uv; p->glitch=g.glitch;
  p->memories=g.memories; p->ammo=g.ammo; p->intensity=g.intensity;
  p->photoCount=wallTextureCount; p->selectedPhoto=g.selectedWall; p->nextPhoto=wallTextureNext;
  for (int i=0; i<MAX_ENTITIES; ++i) if (!entities[i].active) p->inactive |= 1u << i;
  if(!validPatch(*p)){free(p);return false;}
  uint32_t a=inspectPatch(slot,0), b=inspectPatch(slot,1);
  int bank = a > b ? 1 : 0;
  uint32_t generation=max(a,b)+1;
  if (!generation) { free(p); return false; }
  WorldHeader h={0x50555742u,1,generation,sizeof(PatchData),
    checksumBytes(2166136261u,(const uint8_t*)p,sizeof(PatchData))};
  char path[32]; patchPath(path,sizeof(path),slot,bank);
  File f=SD_MMC.open(path,"w");
  bool ok=f && f.write((const uint8_t*)&h,sizeof(h))==sizeof(h) &&
    f.write((const uint8_t*)p,sizeof(PatchData))==sizeof(PatchData);
  if(f){f.flush();f.close();} free(p);
  return ok && inspectPatch(slot,bank)==generation;
}

bool bwoGameLoadPatch(uint8_t slot) {
  if (!worldSdReady || slot >= 4 || !frameBuf) return false;
  uint32_t a=inspectPatch(slot,0), b=inspectPatch(slot,1);
  if (!a && !b) return false;
  PatchData* p=(PatchData*)heap_caps_malloc(sizeof(PatchData),MALLOC_CAP_SPIRAM);
  if(!p) p=(PatchData*)malloc(sizeof(PatchData));
  if(!p) return false;
  bool ok=false; int first=b>a?1:0;
  for(int attempt=0;attempt<2&&!ok;attempt++) {
    int bank=attempt?1-first:first;
    if (!(bank?b:a)) continue;
    char path[32]; patchPath(path,sizeof(path),slot,bank);
    File f=SD_MMC.open(path,FILE_READ);
    WorldHeader h={};
    ok=f && f.read((uint8_t*)&h,sizeof(h))==sizeof(h) &&
       h.magic==0x50555742u && h.version==1 &&
       h.length==sizeof(PatchData) && h.generation==(bank?b:a) &&
       f.read((uint8_t*)p,sizeof(PatchData))==sizeof(PatchData) &&
       h.checksum==checksumBytes(2166136261u,(const uint8_t*)p,sizeof(PatchData)) && validPatch(*p);
    if(f) f.close();
  }
  if(!ok){free(p);return false;}
  if(p->photoCount && !wallTextures) {
    wallTextures=(uint16_t*)heap_caps_malloc(sizeof(p->photos),MALLOC_CAP_SPIRAM);
    if(!wallTextures) wallTextures=(uint16_t*)malloc(sizeof(p->photos));
    if(!wallTextures){free(p);return false;}
  }
  memcpy(builtWalls,p->walls,sizeof(builtWalls));
  memcpy(faceMaterials,p->faces,sizeof(faceMaterials));
  if(wallTextures) memcpy(wallTextures,p->photos,sizeof(p->photos));
  wallTextureCount=p->photoCount; wallTextureNext=p->nextPhoto;
  g=Game(); g.x=p->x;g.y=p->y;g.angle=p->angle;g.level=p->level;
  g.theme=p->theme;g.uv=p->uv;g.glitch=p->glitch;g.memories=p->memories;
  g.ammo=p->ammo;g.intensity=p->intensity;g.selectedWall=p->selectedPhoto;
  makeWorld();
  for(int i=0;i<MAX_ENTITIES;i++) entities[i].active=!(p->inactive&(1u<<i));
  free(p);
  if(!openAt(g.x,g.y)) {
    bool found=false;
    for(int y=1;y<MAP_H-1&&!found;y++) for(int x=1;x<MAP_W-1;x++)
      if(openAt(x+.5f,y+.5f)){g.x=x+.5f;g.y=y+.5f;found=true;break;}
  }
  g.over=!g.intensity;g.lastTick=millis();
  worldDirty=true;worldDirtyAt=millis();
  return true;
}

void bwoGameTouch(int x, int y, bool down) {
  if (!down) {
    g.touching = false;
    g.move = 0.0f;
    g.turn = 0.0f;
    return;
  }
  if (!g.touching) {
    g.anchorX = x;
    g.anchorY = y;
    g.touching = true;
  }
  g.touchX = x;
  g.touchY = y;
  g.turn = constrain((x - g.anchorX) / 45.0f, -1.0f, 1.0f);
  g.move = constrain((g.anchorY - y) / 50.0f, -1.0f, 1.0f);
}

void bwoGameFire() { fireWeapon(); }
void bwoGameTogglePause() { if (!g.over) g.paused = !g.paused; }
bool bwoGamePaused() { return g.paused; }
bool bwoGameReady() { return frameBuf != nullptr; }
void bwoGameToggleBuild() {
  g.building = !g.building;
  g.paused = false;
}
bool bwoGameBuildMode() { return g.building; }

bool bwoGamePlaceWall() {
  if (!g.building) return false;
  int lastX = (int)g.x, lastY = (int)g.y;
  for (float d = 0.35f; d <= 2.0f; d += 0.10f) {
    int x = (int)(g.x + cosf(g.angle) * d);
    int y = (int)(g.y + sinf(g.angle) * d);
    uint8_t tile = tileAt(x, y);
    if (tile) {
      if (lastX == (int)g.x && lastY == (int)g.y) return false;
      break;
    }
    lastX = x; lastY = y;
  }
  if (lastX <= 0 || lastY <= 0 || lastX >= MAP_W - 1 || lastY >= MAP_H - 1 ||
      (lastX == (int)g.x && lastY == (int)g.y)) return false;
  for (const Entity& e : entities)
    if (e.active && e.level == g.level &&
        (int)e.x == lastX && (int)e.y == lastY) return false;
  int i = cellIndex(g.level, lastX, lastY);
  uint8_t material = wallTextureCount ? (g.selectedWall % wallTextureCount) + 1 : 0;
  mapData[i] = (uint8_t)(0x80 | material);
  builtWalls[i] = mapData[i];
  faceMaterials[i] = 0;
  for (int face = 0; face < 4; ++face)
    faceMaterials[i] |= (uint32_t)material << (face * 3);
  worldDirty = true; worldDirtyAt = millis();
  return true;
}

bool bwoGameRemoveWall() {
  if (!g.building) return false;
  for (float d = 0.2f; d <= 2.5f; d += 0.08f) {
    int x = (int)(g.x + cosf(g.angle) * d), y = (int)(g.y + sinf(g.angle) * d);
    if (!tileAt(x, y)) continue;
    if (x <= 0 || y <= 0 || x >= MAP_W-1 || y >= MAP_H-1) return false;
    int i = cellIndex(g.level, x, y);
    builtWalls[i] = 0x40;  // Tombstone also removes generated interior walls.
    mapData[i] = 0; faceMaterials[i] = 0;
    worldDirty = true; worldDirtyAt = millis();
    return true;
  }
  return false;
}

uint8_t bwoGameTheme() { return g.theme; }
uint8_t bwoGameUv() { return g.uv; }
uint8_t bwoGameGlitch() { return g.glitch; }
uint8_t bwoGameFragments() { return g.memories; }
bool bwoGameCycleTheme() {
  g.theme = (g.theme + 1) % 4;
  return true;
}
void bwoGameCycleUv() { g.uv = (g.uv + 1) & 3; }
void bwoGameCycleGlitch() { g.glitch = (g.glitch + 1) & 3; }

void bwoGameCycleWallTexture() {
  if (wallTextureCount) g.selectedWall = (g.selectedWall + 1) % wallTextureCount;
}

bool bwoGameSelectWallTexture(uint8_t slot) {
  if (slot >= wallTextureCount) return false;
  g.selectedWall = slot;
  return true;
}

uint8_t bwoGameSelectedWallTexture() { return g.selectedWall; }

uint8_t bwoGameLevel() { return g.level; }

bool bwoGameChangeLevel(int direction) {
  int next = (int)g.level + direction;
  if (next < 0 || next >= BWO_LEVELS) return false;
  uint8_t old = g.level;
  g.level = next;
  if (!openAt(g.x, g.y)) {
    bool found = false;
    for (int radius = 1; radius <= 3 && !found; ++radius) {
      for (int dy = -radius; dy <= radius && !found; ++dy)
        for (int dx = -radius; dx <= radius; ++dx) {
          float x = floorf(g.x) + dx + 0.5f;
          float y = floorf(g.y) + dy + 0.5f;
          if (openAt(x, y)) { g.x = x; g.y = y; found = true; break; }
        }
    }
    if (!found) { g.level = old; return false; }
  }
  g.touching = false; g.move = 0; g.turn = 0;
  g.lastTick = millis();
  return true;
}

// The tool now has three targets, not six: WALL (whichever face is aimed at),
// then the floor and the ceiling of the cell underfoot. Compass faces were a
// manual substitute for aiming; the four wall values all mean "aimed wall".
void bwoGameCycleFace() {
  g.selectedFace = g.selectedFace < BWO_FACE_FLOOR ? BWO_FACE_FLOOR :
                   g.selectedFace == BWO_FACE_FLOOR ? BWO_FACE_CEILING :
                   BWO_FACE_NORTH;
}

BwoFace bwoGameSelectedFace() { return g.selectedFace; }

/* Finds the wall face under the crosshair with the SAME ray-march the renderer
 * uses, so "the face you are looking at" means exactly what the screen shows.
 *
 * This replaces a scan that only accepted blocks the player had built
 * (tile & 0x80). Almost every wall in the world is generated, not built, so the
 * paint tool refused nearly every surface a person could aim at -- photographs
 * could only ever land on your own blocks. Nothing else stood in the way: the
 * renderer already reads per-face materials for any cell it hits, and the world
 * file and the patch validator already store and accept them for every cell. */
static bool aimedWallFace(int* outX, int* outY, BwoFace* outFace, float maxDist) {
  const float dirX = cosf(g.angle), dirY = sinf(g.angle);
  int mapX = (int)g.x, mapY = (int)g.y;
  const float deltaX = fabsf(dirX) < 0.0001f ? 1e9f : fabsf(1.0f / dirX);
  const float deltaY = fabsf(dirY) < 0.0001f ? 1e9f : fabsf(1.0f / dirY);
  const int stepX = dirX < 0 ? -1 : 1;
  const int stepY = dirY < 0 ? -1 : 1;
  float sideX = dirX < 0 ? (g.x - mapX) * deltaX : (mapX + 1.0f - g.x) * deltaX;
  float sideY = dirY < 0 ? (g.y - mapY) * deltaY : (mapY + 1.0f - g.y) * deltaY;
  for (int depth = 0; depth < 16; ++depth) {
    int side;
    float travelled;
    if (sideX < sideY) { travelled = sideX; sideX += deltaX; mapX += stepX; side = 0; }
    else               { travelled = sideY; sideY += deltaY; mapY += stepY; side = 1; }
    if (travelled > maxDist) return false;
    // setFaceMaterial indexes the world array unchecked, so never hand it a
    // cell outside the map -- tileAt() reports out-of-range cells as solid.
    if (mapX < 0 || mapY < 0 || mapX >= MAP_W || mapY >= MAP_H) return false;
    if (tileAt(mapX, mapY)) {
      *outX = mapX; *outY = mapY;
      *outFace = side == 0 ? (stepX > 0 ? BWO_FACE_WEST : BWO_FACE_EAST)
                           : (stepY > 0 ? BWO_FACE_NORTH : BWO_FACE_SOUTH);
      return true;
    }
  }
  return false;
}

bool bwoGamePaintFace() {
  if (!g.building || !wallTextureCount) return false;
  const uint8_t material = (g.selectedWall % wallTextureCount) + 1;
  if (g.selectedFace >= BWO_FACE_FLOOR) {      // floor / ceiling of this cell
    setFaceMaterial(g.level, (int)g.x, (int)g.y, g.selectedFace, material);
    Serial.printf("[BWO] painted %s of %d,%d with photo %u\n",
                  g.selectedFace == BWO_FACE_FLOOR ? "floor" : "ceiling",
                  (int)g.x, (int)g.y, (unsigned)material);
    return true;
  }
  int cx = 0, cy = 0;
  BwoFace face = BWO_FACE_NORTH;
  if (!aimedWallFace(&cx, &cy, &face, 3.0f)) return false;
  setFaceMaterial(g.level, cx, cy, face, material);
  Serial.printf("[BWO] painted wall %d,%d face %d with photo %u\n",
                cx, cy, (int)face, (unsigned)material);
  return true;
}

bool bwoGameAddWallTexture(const uint16_t* pixels, int width, int height,
                           int stride) {
  if (!pixels || width < 1 || height < 1 || stride < width) return false;
  if (!wallTextures) {
    size_t bytes = (size_t)BWO_WALL_TEXTURE_SLOTS * WALL_TEX_PIXELS *
                   sizeof(uint16_t);
    wallTextures = static_cast<uint16_t*>(
      heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
    if (!wallTextures)
      wallTextures = static_cast<uint16_t*>(
        heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL));
    if (!wallTextures) return false;
    memset(wallTextures, 0, bytes);
  }

  // Center-crop to a square before reducing. This avoids stretching portrait
  // captures across an entire wall face.
  int crop = min(width, height);
  int ox = (width - crop) / 2;
  int oy = (height - crop) / 2;
  uint16_t* dst = wallTextures + wallTextureNext * WALL_TEX_PIXELS;
  for (int y = 0; y < WALL_TEX_H; ++y) {
    int sy = oy + (y * crop + crop / (WALL_TEX_H * 2)) / WALL_TEX_H;
    sy = constrain(sy, 0, height - 1);
    for (int x = 0; x < WALL_TEX_W; ++x) {
      int sx = ox + (x * crop + crop / (WALL_TEX_W * 2)) / WALL_TEX_W;
      sx = constrain(sx, 0, width - 1);
      dst[y * WALL_TEX_W + x] = pixels[(size_t)sy * stride + sx];
    }
  }
  g.selectedWall = wallTextureNext;
  wallTextureNext = (wallTextureNext + 1) % BWO_WALL_TEXTURE_SLOTS;
  if (wallTextureCount < BWO_WALL_TEXTURE_SLOTS) ++wallTextureCount;
  return true;
}

void bwoGameClearWallTextures() {
  wallTextureCount = 0;
  wallTextureNext = 0;
}

uint8_t bwoGameWallTextureCount() { return wallTextureCount; }

const uint16_t* bwoGameWallTexture(uint8_t slot) {
  return wallTextures && slot < wallTextureCount ?
    wallTextures + slot * WALL_TEX_PIXELS : nullptr;
}

const uint16_t* bwoGameFrame(uint32_t nowMs) {
  if (!frameBuf) return nullptr;
  LinkGamePeer shared[LINK_MAX_PEERS];
  size_t count = linkGamePeers(shared, LINK_MAX_PEERS);
  for (size_t i = 0; i < count; ++i) {
    for (int frag = 0; frag < 7; ++frag) {
      if ((shared[i].pose.fragments & (1u << frag)) && entities[frag].active) {
        entities[frag].active = false;
        ++g.memories;
        g.resonance = (uint8_t)min(100, (int)g.resonance + 15);
      }
    }
  }
  if (!g.lastTick) g.lastTick = nowMs;
  uint32_t elapsed = nowMs - g.lastTick;
  if (elapsed > 120) elapsed = 120;
  while (elapsed >= 33) {
    updateGame(0.033f);
    ++g.ticks;
    g.lastTick += 33;
    elapsed -= 33;
  }
  if (worldDirty && nowMs - worldDirtyAt > worldSaveDelay) {
    bool saved = saveWorldSlot();
    if (saved) { worldDirty = false; worldSaveDelay = 1500; }
    else {
      Serial.println("[BWO] world save deferred: SD unavailable or verify failed");
      worldDirtyAt = nowMs; worldSaveDelay = 5000;
    }
  }
  float angle = g.angle < 0 ? g.angle + 2.0f * PI_F : g.angle;
  LinkGamePose pose = {
    (uint16_t)constrain((int)(g.x * 100.0f), 0, 2399),
    (uint16_t)constrain((int)(g.y * 100.0f), 0, 2399),
    (uint16_t)constrain((int)(angle * 1000.0f), 0, 6284),
    g.level,
    0
  };
  for (int frag = 0; frag < 7; ++frag)
    if (!entities[frag].active) pose.fragments |= (1u << frag);
  linkGamePublish(pose);
  renderWorld();
  return frameBuf;
}
