/* ── WUW-01 / PASAR PROTOCOL — exhibition state ───────────────────────────
 * See pasar.h for why room state is pulled rather than pushed.
 *
 * Time: this device has no RTC and no internet, so it cannot know the date.
 * The spec says to record uptime and sequence numbers honestly rather than
 * invent a wall clock, so sessions are numbered (PASAR_S0007) and every
 * timestamp emitted is milliseconds since boot, labelled as such. A browser's
 * clock may be recorded alongside, but only ever tagged with its provenance.
 */
#ifdef EXHIBITION_MODE

#include "pasar.h"
#include <Preferences.h>
#include "SD_MMC.h"

static Preferences pprefs;

static uint32_t sessSeq   = 0;
static char     sessName[20] = "PASAR_S0000";
static char     sessDir[24]  = "/PASAR_S0000";
static uint32_t sessStartMs  = 0;

static uint32_t photoCountSession = 0;
static uint32_t lastImage = 0;
static char     lastAuthor[PASAR_ALIAS_MAX + 2] = "";

struct Visitor {
  char     alias[PASAR_ALIAS_MAX + 2];
  uint32_t lastSeen;
  bool     used;
};
static Visitor vis[PASAR_MAX_VISITORS];
static uint32_t visSerial = 0;          // for generated visitor-NNN names

/* Aliases become HTML on other people's phones and filenames in the archive,
   so they are filtered down to a conservative set here rather than escaped
   later. Anything outside it is dropped, not substituted, so a hostile alias
   simply shrinks instead of becoming something surprising. */
void pasarSanitizeAlias(const char* in, char* out, size_t outN) {
  size_t j = 0;
  if (in) {
    for (size_t i = 0; in[i] && j + 1 < outN && j < PASAR_ALIAS_MAX; i++) {
      char c = in[i];
      bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ' ';
      if (ok) out[j++] = c;
    }
    while (j && out[j-1] == ' ') j--;          // no trailing space
  }
  out[j] = 0;
}

static void makeSessionNames() {
  snprintf(sessName, sizeof(sessName), "PASAR_S%04u", (unsigned)sessSeq);
  snprintf(sessDir,  sizeof(sessDir),  "/PASAR_S%04u", (unsigned)sessSeq);
}

static void writeSessionJson(bool closing) {
  if (!SD_MMC.cardType()) return;
  SD_MMC.mkdir(sessDir);
  char path[48];
  snprintf(path, sizeof(path), "%s/session.json", sessDir);
  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) return;
  f.printf("{\"device\":\"WUW-01\",\"protocol\":\"PASAR\","
           "\"session\":\"%s\",\"seq\":%u,"
           "\"startedUptimeMs\":%u,\"uptimeMs\":%u,"
           "\"images\":%u,\"closed\":%s,"
           "\"clock\":\"none - device has no RTC; values are ms since boot\"}\n",
           sessName, (unsigned)sessSeq, (unsigned)sessStartMs,
           (unsigned)millis(), (unsigned)photoCountSession,
           closing ? "true" : "false");
  f.close();
}

void pasarBegin() {
  /* A session used to start on every boot. On hardware that turned out to be
     wrong: a power blip -- or a reflash -- orphaned every photograph taken so
     far into a folder the archive no longer shows. An exhibition runs for
     hours and will lose power at least once, so the session now RESUMES
     unless an admin explicitly closed it. */
  pprefs.begin("pasar", false);
  sessSeq = pprefs.getUInt("seq", 0);
  bool closed = pprefs.getBool("closed", true);
  if (sessSeq == 0 || closed) {
    sessSeq++;
    pprefs.putUInt("seq", sessSeq);
    pprefs.putBool("closed", false);
    Serial.println("[PASAR] starting a new session");
  } else {
    Serial.println("[PASAR] resuming the open session after reboot");
  }
  photoCountSession = pprefs.getUInt("shots", 0);   // survives the reboot too
  pprefs.end();
  makeSessionNames();
  sessStartMs = millis();
  lastImage = 0;
  lastAuthor[0] = 0;
  memset(vis, 0, sizeof(vis));
  visSerial = 0;
  writeSessionJson(false);
  Serial.printf("[PASAR] session %s (dir %s)\n", sessName, sessDir);
}

bool pasarNewSession() {
  writeSessionJson(true);                       // close the outgoing one
  pprefs.begin("pasar", false);
  sessSeq = pprefs.getUInt("seq", 0) + 1;
  pprefs.putUInt("seq", sessSeq);
  pprefs.putBool("closed", false);
  pprefs.putUInt("shots", 0);
  pprefs.end();
  makeSessionNames();
  sessStartMs = millis();
  photoCountSession = 0;
  lastImage = 0;
  lastAuthor[0] = 0;
  writeSessionJson(false);
  Serial.printf("[PASAR] new session %s\n", sessName);
  return true;
}

const char* pasarSession()     { return sessName; }
const char* pasarSessionDir()  { return sessDir; }
uint32_t    pasarSessionSeq()  { return sessSeq; }
uint32_t    pasarPhotoCount()  { return photoCountSession; }
uint32_t    pasarLastImage()   { return lastImage; }
const char* pasarLastAuthor()  { return lastAuthor; }
uint32_t    pasarUptimeMs()    { return millis() - sessStartMs; }

static void reap(uint32_t now) {
  for (int i = 0; i < PASAR_MAX_VISITORS; i++)
    if (vis[i].used && (now - vis[i].lastSeen) > PASAR_STALE_MS)
      vis[i].used = false;
}

int pasarCount() {
  uint32_t now = millis();
  reap(now);
  int n = 0;
  for (int i = 0; i < PASAR_MAX_VISITORS; i++) if (vis[i].used) n++;
  return n;
}

int pasarTouch(const char* aliasIn, char* out, size_t outN) {
  uint32_t now = millis();
  reap(now);

  char want[PASAR_ALIAS_MAX + 2];
  pasarSanitizeAlias(aliasIn, want, sizeof(want));

  int slot = -1, freeSlot = -1;
  for (int i = 0; i < PASAR_MAX_VISITORS; i++) {
    if (!vis[i].used) { if (freeSlot < 0) freeSlot = i; continue; }
    if (want[0] && !strcmp(vis[i].alias, want)) { slot = i; break; }
  }
  if (slot < 0) {
    if (freeSlot < 0) {                       // room full: recycle the oldest
      uint32_t oldest = 0xFFFFFFFFu;
      for (int i = 0; i < PASAR_MAX_VISITORS; i++)
        if (vis[i].lastSeen < oldest) { oldest = vis[i].lastSeen; freeSlot = i; }
    }
    slot = freeSlot;
    if (!want[0]) snprintf(want, sizeof(want), "visitor-%03u",
                           (unsigned)((++visSerial) % 1000));
    vis[slot].used = true;
    snprintf(vis[slot].alias, sizeof(vis[slot].alias), "%s", want);
    Serial.printf("[PASAR] join %s\n", vis[slot].alias);
  }
  vis[slot].lastSeen = now;
  if (out && outN) snprintf(out, outN, "%s", vis[slot].alias);

  int n = 0;
  for (int i = 0; i < PASAR_MAX_VISITORS; i++) if (vis[i].used) n++;
  return n;
}

void pasarShutter(uint32_t imgNum, const char* alias) {
  photoCountSession++;
  { Preferences hw; hw.begin("pasar", false);
    hw.putUInt("shots", photoCountSession); hw.end(); }
  lastImage = imgNum;
  pasarSanitizeAlias(alias, lastAuthor, sizeof(lastAuthor));
  if (!lastAuthor[0]) snprintf(lastAuthor, sizeof(lastAuthor), "someone");
  writeSessionJson(false);
}

void pasarNoteWritten(uint32_t imgNum) { (void)imgNum; }

/* JSON assembled into a fixed buffer rather than String concatenation: this is
   fetched by every visitor every couple of seconds, and String growth in a hot
   path is exactly how the heap fragments over a multi-hour exhibition. */
String pasarRoomJson(bool camOk, bool sdOk, const char* devName) {
  char buf[352];
  int n = pasarCount();
  snprintf(buf, sizeof(buf),
    "{\"exhibition\":true,\"device\":\"%s\",\"protocol\":\"PASAR\","
    "\"session\":\"%s\",\"visitors\":%d,\"photos\":%u,"
    "\"lastImage\":%u,\"lastAuthor\":\"%s\","
    "\"sessionUptimeMs\":%u,\"camera\":%s,\"sd\":%s}",
    (devName && *devName) ? devName : "WUW-01",
    sessName, n, (unsigned)photoCountSession,
    (unsigned)lastImage, lastAuthor, (unsigned)pasarUptimeMs(),
    camOk ? "true" : "false", sdOk ? "true" : "false");
  return String(buf);
}

#endif /* EXHIBITION_MODE */
