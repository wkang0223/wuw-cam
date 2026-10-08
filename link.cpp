/* See link.h. Discovery, synchronised shutter, and pulling frames off a peer. */
#ifdef WUW_LINK

#include "link.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_now.h>
#include <esp_idf_version.h>
#include <SD_MMC.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

/* ── state ─────────────────────────────────────────────────────────────── */

struct Peer {
  char     id[9]      = {0};
  char     name[24]   = {0};
  char     session[16]= {0};
  IPAddress ip;
  uint32_t photos     = 0;
  uint8_t  flags      = 0;   // b0 sd, b1 cam
  uint32_t seen       = 0;
  bool     used       = false;
  uint8_t  mac[6]     = {0};
  bool     hasMac     = false;
  LinkGamePose game   = {};
  uint32_t gameSeen   = 0;
  uint32_t gameSeq    = 0;
};

static Peer      peers[LINK_MAX_PEERS];
static SemaphoreHandle_t peerMux = nullptr;

static WiFiUDP   udp;
static bool      up = false;
static bool      udpUp = false;
static bool      nowUp = false;
static QueueHandle_t nowQ = nullptr;
static const uint8_t nowBroadcast[6] = {255,255,255,255,255,255};
static portMUX_TYPE nowMux = portMUX_INITIALIZER_UNLOCKED;
static bool nowSending = false;
static uint32_t nowSentAt = 0;
struct NowPacket { char data[200]; uint8_t mac[6]; };
static LinkGamePose localGame = {};
static bool localGameActive = false;
static uint32_t localGameSeq = 0;
static char      selfId[9]   = "00000000";
static char      group[16]   = "wuw";
static LinkSelf  selfInfo    = { "WUW", "", 0, false, false };
static LinkShootFn shootFn   = nullptr;

static volatile uint32_t fireAt  = 0;      // millis() deadline, 0 = none
static char      fireTag[16]     = "";

/* ── helpers ───────────────────────────────────────────────────────────── */

/* Fields are used in both the pipe protocol and JSON responses. Replace the
   protocol separator, JSON delimiters and controls at ingestion. */
static void clean(char* dst, size_t cap, const char* src) {
  size_t i = 0;
  for (; src && src[i] && i < cap - 1; i++)
    dst[i] = (src[i] == '|' || src[i] == '"' || src[i] == '\\' || src[i] < 32)
                 ? '-' : src[i];
  dst[i] = 0;
}

static void sendTo(IPAddress bc, const char* line) {
  if (udp.beginPacket(bc, WUW_LINK_PORT)) {
    udp.write((const uint8_t*)line, strlen(line));
    udp.endPacket();
  }
}

/* Both subnets. The AP one reaches a second camera joined to ours; the STA
   one reaches every camera on the venue's router. Which of those exists is
   not this module's business, so it tries both and ignores failures. */
static void broadcast(const char* line, bool nearby = true) {
  if (!up) return;
  if (udpUp) {
    IPAddress ap = WiFi.softAPIP();
    if ((uint32_t)ap) { ap[3] = 255; sendTo(ap, line); }
    if (WiFi.status() == WL_CONNECTED) {
      IPAddress ip = WiFi.localIP(), mask = WiFi.subnetMask();
      IPAddress bc((uint32_t)ip | ~(uint32_t)mask);
      sendTo(bc, line);
    }
  }
  if (nearby && nowUp) {
    bool send = false;
    uint32_t now = millis();
    portENTER_CRITICAL(&nowMux);
    if (!nowSending || now - nowSentAt > 500) {
      nowSending = true;
      nowSentAt = now;
      send = true;
    }
    portEXIT_CRITICAL(&nowMux);
    if (send && esp_now_send(nowBroadcast, (const uint8_t*)line,
                             strlen(line)) != ESP_OK) {
      portENTER_CRITICAL(&nowMux);
      nowSending = false;
      portEXIT_CRITICAL(&nowMux);
    }
  }
}

static void beacon() {
  char nm[24], ss[16], b[160];
  clean(nm, sizeof(nm), selfInfo.name);
  clean(ss, sizeof(ss), selfInfo.session);
  snprintf(b, sizeof(b), "WUWL1|%s|HELLO|%s|%s|%s|%u|%u",
           group, selfId, nm, ss, (unsigned)selfInfo.photos,
           (unsigned)((selfInfo.sd ? 1 : 0) | (selfInfo.cam ? 2 : 0)));
  broadcast(b);
}

/* ── peer table ────────────────────────────────────────────────────────── */

static void peerSeen(const char* id, const char* name, const char* sess,
                     uint32_t photos, uint8_t flags, IPAddress from,
                     const uint8_t* mac) {
  if (!peerMux || xSemaphoreTake(peerMux, pdMS_TO_TICKS(20)) != pdTRUE) return;
  int slot = -1, freeSlot = -1;
  for (int i = 0; i < LINK_MAX_PEERS; i++) {
    if (peers[i].used && !strcmp(peers[i].id, id)) { slot = i; break; }
    if (!peers[i].used && freeSlot < 0) freeSlot = i;
  }
  if (slot < 0) {
    slot = freeSlot;
    if (slot < 0) {                       /* full: evict the stalest */
      uint32_t worst = 0xFFFFFFFF;
      for (int i = 0; i < LINK_MAX_PEERS; i++)
        if (peers[i].seen < worst) { worst = peers[i].seen; slot = i; }
    }
    peers[slot] = Peer();          // IPAddress is a class; memset is not its friend
  }
  Peer& p = peers[slot];
  if (p.gameSeen && millis() - p.gameSeen > 1500) {
    p.gameSeen = 0;
    p.gameSeq = 0;
  }
  p.used = true;
  clean(p.id, sizeof(p.id), id);
  clean(p.name, sizeof(p.name), name);
  clean(p.session, sizeof(p.session), sess);
  if ((uint32_t)from) p.ip = from;
  if (mac) { memcpy(p.mac, mac, 6); p.hasMac = true; }
  p.photos = photos; p.flags = flags; p.seen = millis();
  xSemaphoreGive(peerMux);
}

static void peerExpire() {
  if (!peerMux || xSemaphoreTake(peerMux, pdMS_TO_TICKS(20)) != pdTRUE) return;
  uint32_t now = millis();
  for (int i = 0; i < LINK_MAX_PEERS; i++)
    if (peers[i].used && now - peers[i].seen > LINK_STALE_MS) peers[i].used = false;
  xSemaphoreGive(peerMux);
}

int linkPeerCount() {
  int n = 0;
  if (!peerMux || xSemaphoreTake(peerMux, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
  for (int i = 0; i < LINK_MAX_PEERS; i++) if (peers[i].used) n++;
  xSemaphoreGive(peerMux);
  return n;
}

void linkGamePublish(const LinkGamePose& pose) {
  if (!peerMux || xSemaphoreTake(peerMux, pdMS_TO_TICKS(2)) != pdTRUE) return;
  localGame = pose;
  localGameActive = true;
  xSemaphoreGive(peerMux);
}

void linkGameStop() {
  if (!peerMux || xSemaphoreTake(peerMux, pdMS_TO_TICKS(5)) != pdTRUE) return;
  localGameActive = false;
  xSemaphoreGive(peerMux);
}

size_t linkGamePeers(LinkGamePeer* out, size_t cap) {
  if (!out || !cap || !peerMux ||
      xSemaphoreTake(peerMux, pdMS_TO_TICKS(2)) != pdTRUE) return 0;
  size_t n = 0;
  uint32_t now = millis();
  for (int i = 0; i < LINK_MAX_PEERS && n < cap; ++i) {
    const Peer& p = peers[i];
    if (!p.used || !p.gameSeen || now - p.gameSeen > 1500) continue;
    memcpy(out[n].id, p.id, sizeof(p.id));
    out[n].pose = p.game;
    out[n].seen = p.gameSeen;
    ++n;
  }
  xSemaphoreGive(peerMux);
  return n;
}

/* snprintf returns what it WANTED to write, not what it did. Accumulating
   that return value lets w run past cap; the next call then computes
   `cap - w` on a size_t, which wraps to about four billion, and writes
   happily off the end of the buffer.
 
   Every field feeding this comes off the wire -- a peer's name is whatever
   that device says it is -- so the overflow is reachable by anyone who can
   send a beacon. It is worth being blunt about the shape of the mistake,
   because it compiles, it passes every test with two cameras in the room,
   and it only detonates when the table is full of long names.
 
   Fix in two parts: append through something that cannot advance past the
   buffer, and commit each peer only if it fits WHOLE. Truncating a peer
   would leave half an object and the browser would throw on the JSON. */
static bool jappend(char* out, size_t cap, size_t& w, const char* src) {
  size_t n = strlen(src);
  if (w + n >= cap) return false;            // no room: caller decides
  memcpy(out + w, src, n);
  w += n;
  out[w] = 0;
  return true;
}

size_t linkPeersJson(char* out, size_t cap) {
  if (!out || cap < 128) return 0;
  size_t w = 0;
  out[0] = 0;

  char head[128];
  snprintf(head, sizeof(head),
           "{\"ok\":true,\"up\":%s,\"group\":\"%s\",\"me\":\"%s\",\"peers\":[",
           up ? "true" : "false", group, selfId);
  jappend(out, cap, w, head);

  /* Reserve the tail up front so there is always room to close the JSON. */
  char tail[128];
  snprintf(tail, sizeof(tail), "],\"pull\":\"%s\"}", linkPullStatus());
  const size_t reserve = strlen(tail);

  int shown = 0, total = 0;
  if (peerMux && xSemaphoreTake(peerMux, pdMS_TO_TICKS(50)) == pdTRUE) {
    uint32_t now = millis();
    for (int i = 0; i < LINK_MAX_PEERS; i++) {
      if (!peers[i].used) continue;
      total++;
      char one[256];
      snprintf(one, sizeof(one),
               "%s{\"id\":\"%.8s\",\"name\":\"%.23s\",\"ip\":\"%.15s\",\"ses\":\"%.15s\","
               "\"photos\":%u,\"sd\":%s,\"age\":%u,\"radio\":%s,\"playing\":%s}",
               shown ? "," : "", peers[i].id, peers[i].name,
               peers[i].ip.toString().c_str(), peers[i].session,
               (unsigned)peers[i].photos,
               (peers[i].flags & 1) ? "true" : "false",
               (unsigned)((now - peers[i].seen) / 1000),
               peers[i].hasMac ? "true" : "false",
               peers[i].gameSeen && now - peers[i].gameSeen < 1500 ? "true" : "false");
      if (w + strlen(one) + reserve >= cap) break;   // whole entry or none
      jappend(out, cap, w, one);
      shown++;
    }
    xSemaphoreGive(peerMux);
  }
  (void)total;
  jappend(out, cap, w, tail);                        // reserved: always fits
  return w;
}

/* ── inbound ───────────────────────────────────────────────────────────── */

static bool parseNumber(const char* s, uint32_t maxValue, uint32_t& value) {
  if (!s || !*s) return false;
  uint32_t n = 0;
  for (; *s; ++s) {
    if (*s < '0' || *s > '9') return false;
    uint8_t digit = *s - '0';
    if (digit > maxValue || n > (maxValue - digit) / 10) return false;
    n = n * 10 + digit;
  }
  value = n;
  return true;
}

static void handle(char* pkt, IPAddress from, const uint8_t* mac) {
  /* WUWL1|group|VERB|id|... — anything that does not match is dropped in
     silence. There is no error path back to a sender we do not trust. */
  char* save = nullptr;
  char* magic = strtok_r(pkt,  "|", &save); if (!magic || strcmp(magic, "WUWL1")) return;
  char* grp   = strtok_r(NULL, "|", &save); if (!grp   || strcmp(grp, group))     return;
  char* verb  = strtok_r(NULL, "|", &save); if (!verb) return;
  char* id    = strtok_r(NULL, "|", &save); if (!id || !*id) return;
  if (strlen(id) != 8) return;
  for (const char* c = id; *c; ++c)
    if (!((*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'F'))) return;
  if (!strcmp(id, selfId)) return;                    // our own broadcast

  if (!strcmp(verb, "HELLO")) {
    char* nm = strtok_r(NULL, "|", &save);
    char* ss = strtok_r(NULL, "|", &save);
    char* ph = strtok_r(NULL, "|", &save);
    char* fl = strtok_r(NULL, "|", &save);
    peerSeen(id, nm ? nm : "?", ss ? ss : "", ph ? strtoul(ph, nullptr, 10) : 0,
             fl ? (uint8_t)atoi(fl) : 0, from, mac);
    return;
  }
  if (!strcmp(verb, "BWO")) {
    char* seq = strtok_r(NULL, "|", &save);
    char* lev = strtok_r(NULL, "|", &save);
    char* x = strtok_r(NULL, "|", &save);
    char* y = strtok_r(NULL, "|", &save);
    char* a = strtok_r(NULL, "|", &save);
    char* fragments = strtok_r(NULL, "|", &save);
    uint32_t sn, ln, xn, yn, an, fn;
    if (!parseNumber(seq, UINT32_MAX, sn) ||
        !parseNumber(lev, 2, ln) || !parseNumber(x, 2399, xn) ||
        !parseNumber(y, 2399, yn) || !parseNumber(a, 6284, an) ||
        !parseNumber(fragments, 127, fn) ||
        !peerMux || xSemaphoreTake(peerMux, pdMS_TO_TICKS(2)) != pdTRUE) return;
    for (Peer& p : peers) {
      if (!p.used || strcmp(p.id, id)) continue;
      if (mac ? (!p.hasMac || memcmp(mac, p.mac, 6)) :
                (!(uint32_t)p.ip || p.ip != from)) break;
      if (!p.gameSeen || (int32_t)(sn - p.gameSeq) > 0) {
        p.game = {(uint16_t)xn, (uint16_t)yn, (uint16_t)an,
                  (uint8_t)ln, (uint8_t)fn};
        p.gameSeq = sn;
        p.gameSeen = millis();
      }
      break;
    }
    xSemaphoreGive(peerMux);
    return;
  }
  if (!strcmp(verb, "FIRE")) {
    if (mac) return;  // nearby discovery must not extend shutter authority
    char* lead = strtok_r(NULL, "|", &save);
    char* tag  = strtok_r(NULL, "|", &save);
    uint32_t l = lead ? strtoul(lead, nullptr, 10) : 250;
    if (l > 5000) l = 5000;                           // no far-future arming
    if (fireAt) return;                               // one pending shot only
    clean(fireTag, sizeof(fireTag), tag ? tag : id);
    fireAt = millis() + l;
    Serial.printf("[LINK] FIRE from %s in %ums\n", id, (unsigned)l);
    return;
  }
}

// Called by the Wi-Fi task: only copy bounded data. Parsing stays on linkTask.
static void nowReceivePacket(const uint8_t* mac, const uint8_t* data, int len) {
  if (!nowQ || !mac || !data || len < 1 || len >= 200) return;
  NowPacket p = {};
  memcpy(p.data, data, len);
  memcpy(p.mac, mac, 6);
  xQueueSend(nowQ, &p, 0);
}

#if ESP_IDF_VERSION_MAJOR >= 5
static void nowReceive(const esp_now_recv_info_t* info,
                       const uint8_t* data, int len) {
  if (info) nowReceivePacket(info->src_addr, data, len);
}
static void nowSent(const wifi_tx_info_t*, esp_now_send_status_t) {
#else
static void nowReceive(const uint8_t* mac, const uint8_t* data, int len) {
  nowReceivePacket(mac, data, len);
}
static void nowSent(const uint8_t*, esp_now_send_status_t) {
#endif
  portENTER_CRITICAL(&nowMux);
  nowSending = false;
  portEXIT_CRITICAL(&nowMux);
}

/* ── the file pull ─────────────────────────────────────────────────────── */

struct PullJob { char ip[16]; int n; char peer[16]; };
static QueueHandle_t pullQ  = nullptr;
static volatile bool pullOn = false;
static char pullStat[72]    = "idle";

bool linkPullBusy()           { return pullOn; }
const char* linkPullStatus()  { return pullStat; }

bool linkPullStart(const char* ip, int n, const char* peerName) {
  if (!pullQ || pullOn || n < 0 || !ip || !*ip) return false;
  PullJob j = {};
  snprintf(j.ip, sizeof(j.ip), "%s", ip);
  clean(j.peer, sizeof(j.peer), peerName && *peerName ? peerName : "peer");
  for (char* c = j.peer; *c; c++)                 // becomes part of a filename
    if (!isalnum((unsigned char)*c) && *c != '-' && *c != '_') *c = '_';
  j.n = n;
  pullOn = true;
  snprintf(pullStat, sizeof(pullStat), "queued %s #%d", j.ip, n);
  if (xQueueSend(pullQ, &j, 0) != pdTRUE) { pullOn = false; return false; }
  return true;
}

static void pullTask(void*) {
  PullJob j;
  for (;;) {
    if (xQueueReceive(pullQ, &j, portMAX_DELAY) != pdTRUE) continue;
    snprintf(pullStat, sizeof(pullStat), "connecting %s", j.ip);

    WiFiClient c;
    c.setTimeout(8);                                   // seconds, per read
    if (!c.connect(j.ip, 80, 6000)) {
      snprintf(pullStat, sizeof(pullStat), "no answer from %s", j.ip);
      pullOn = false; continue;
    }
    c.printf("GET /archive/img?n=%d&full=1 HTTP/1.1\r\n"
             "Host: %s\r\nConnection: close\r\n\r\n", j.n, j.ip);

    /* status line */
    String line = c.readStringUntil('\n');
    if (line.indexOf("200") < 0) {
      snprintf(pullStat, sizeof(pullStat), "peer refused (%.24s)", line.c_str());
      c.stop(); pullOn = false; continue;
    }
    long len = -1;
    while (c.connected()) {                            // headers
      line = c.readStringUntil('\n');
      line.trim();
      if (!line.length()) break;
      if (line.startsWith("Content-Length:")) len = line.substring(15).toInt();
    }

    char path[64];
    snprintf(path, sizeof(path), "/LNK_%s_%04d.jpg", j.peer, j.n);
    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f) {
      snprintf(pullStat, sizeof(pullStat), "cannot write %.40s", path);
      c.stop(); pullOn = false; continue;
    }

    uint8_t buf[1024];
    size_t got = 0;
    uint32_t deadline = millis() + 45000;              // a stalled peer must not hang us
    while (millis() < deadline) {
      int avail = c.available();
      if (avail > 0) {
        int r = c.read(buf, avail > (int)sizeof(buf) ? sizeof(buf) : avail);
        if (r <= 0) break;
        if (f.write(buf, r) != (size_t)r) { got = 0; break; }   // card full
        got += r;
        deadline = millis() + 45000;
        if (len > 0 && (long)got >= len) break;
      } else if (!c.connected()) break;
      else vTaskDelay(pdMS_TO_TICKS(5));
    }
    f.close();
    c.stop();

    bool ok = got > 0 && (len < 0 || (long)got >= len);
    if (!ok) {
      SD_MMC.remove(path);                             // never leave a half frame
      snprintf(pullStat, sizeof(pullStat), "incomplete (%u of %ld)", (unsigned)got, len);
    } else {
      snprintf(pullStat, sizeof(pullStat), "saved %.40s %uKB", path + 1,
                                           (unsigned)(got / 1024));
      Serial.printf("[LINK] pulled %s from %s (%u KB)\n", path, j.ip, (unsigned)(got / 1024));
    }
    pullOn = false;
  }
}

/* ── the task ──────────────────────────────────────────────────────────── */

bool linkFire(uint16_t leadMs, const char* tag) {
  if (!up) return false;
  if (leadMs < 60)   leadMs = 60;
  if (leadMs > 5000) leadMs = 5000;
  char t[16], b[96];
  clean(t, sizeof(t), tag && *tag ? tag : "sync");
  snprintf(b, sizeof(b), "WUWL1|%s|FIRE|%s|%u|%s", group, selfId, (unsigned)leadMs, t);
  broadcast(b, false);
  clean(fireTag, sizeof(fireTag), t);
  fireAt = millis() + leadMs;          // we obey the same countdown we sent
  return true;
}

static void linkTask(void*) {
  uint32_t lastBeacon = 0;
  uint32_t lastGame = 0;
  char pkt[200];
  for (;;) {
    NowPacket np;
    for (int i = 0; i < 4 && xQueueReceive(nowQ, &np, 0) == pdTRUE; ++i)
      handle(np.data, IPAddress(), np.mac);
    int sz = udpUp ? udp.parsePacket() : 0;
    if (sz > 0) {
      int n = udp.read(pkt, sizeof(pkt) - 1);
      if (n > 0 && n == sz) { pkt[n] = 0; handle(pkt, udp.remoteIP(), nullptr); }
    }
    uint32_t now = millis();
    if (now - lastGame >= 100 && peerMux &&
        xSemaphoreTake(peerMux, pdMS_TO_TICKS(2)) == pdTRUE) {
      LinkGamePose pose = localGame;
      bool active = localGameActive;
      xSemaphoreGive(peerMux);
      lastGame = now;
      if (active) {
        char b[96];
        snprintf(b, sizeof(b), "WUWL1|%s|BWO|%s|%u|%u|%u|%u|%u|%u",
                 group, selfId, (unsigned)++localGameSeq,
                 (unsigned)pose.level, (unsigned)pose.x100,
                 (unsigned)pose.y100, (unsigned)pose.angle1000,
                 (unsigned)pose.fragments);
        broadcast(b);
      }
    }
    if (fireAt && (int32_t)(now - fireAt) >= 0) {
      fireAt = 0;
      if (shootFn) shootFn(fireTag);   // serialised shutter; may return false
    }
    if (now - lastBeacon >= LINK_BEACON_MS) {
      lastBeacon = now;
      beacon();
      peerExpire();
    }
    vTaskDelay(pdMS_TO_TICKS(10));     // also the sync-fire granularity
  }
}

/* ── entry points ──────────────────────────────────────────────────────── */

void linkPublish(const LinkSelf& s) { selfInfo = s; }
void linkHooks(LinkShootFn shoot)   { shootFn = shoot; }
bool linkUp()                       { return up; }
const char* linkGroup()             { return group; }

void linkSetGroup(const char* g) {
  if (!g || !*g) return;
  clean(group, sizeof(group), g);
  Preferences p; p.begin("wuw", false); p.putString("lgroup", group); p.end();
  beacon();                            // announce under the new name at once
}

void linkBegin() {
  uint64_t mac = ESP.getEfuseMac();
  snprintf(selfId, sizeof(selfId), "%08X", (unsigned)(mac & 0xFFFFFFFF));

  Preferences p; p.begin("wuw", true);
  String g = p.getString("lgroup", "wuw");
  p.end();
  clean(group, sizeof(group), g.c_str());

  peerMux = xSemaphoreCreateMutex();
  pullQ   = xQueueCreate(1, sizeof(PullJob));
  nowQ    = xQueueCreate(8, sizeof(NowPacket));
  udpUp   = (udp.begin(WUW_LINK_PORT) == 1);
  if (esp_now_init() == ESP_OK) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, nowBroadcast, 6);
    peer.channel = 0;             // current AP/STA channel; no channel switch
    peer.ifidx = WIFI_IF_AP;
    nowUp = esp_now_add_peer(&peer) == ESP_OK &&
            esp_now_register_recv_cb(nowReceive) == ESP_OK &&
            esp_now_register_send_cb(nowSent) == ESP_OK;
    if (!nowUp) esp_now_deinit();
  }
  up = udpUp || nowUp;
  if (!up || !peerMux || !pullQ || !nowQ) {
    Serial.println("[LINK] socket or alloc failed - mesh off");
    up = false;
    return;
  }
  xTaskCreatePinnedToCore(linkTask, "link", 4096, nullptr, 3, nullptr, 0);
  xTaskCreatePinnedToCore(pullTask, "lpull", 6144, nullptr, 2, nullptr, 0);
  Serial.printf("[LINK] %s UDP=%d ESPNOW=%d group \"%s\"\n",
                selfId, udpUp, nowUp, group);
}

#endif
