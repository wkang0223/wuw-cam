/* ── WUW-01 / PASAR PROTOCOL ──────────────────────────────────────────────
 * Exhibition layer. Compiled in only when EXHIBITION_MODE is defined, so the
 * standard firmware is byte-identical without it.
 *
 * Design note that shapes this whole module: the web server is configured for
 * max_open_sockets = 7, and LWIP's own limit is not much higher. A held-open
 * SSE or WebSocket per visitor would exhaust the socket table at the seventh
 * person and begin LRU-purging live connections. The exhibition expects 12-23.
 * So room state is PULLED on the poll clients already make, never pushed. No
 * second protocol, and no socket held open per visitor.
 */
#pragma once
#ifdef EXHIBITION_MODE

#include <Arduino.h>

#define PASAR_MAX_VISITORS 32
#define PASAR_STALE_MS     45000UL     // unseen this long -> left the room
#define PASAR_ALIAS_MAX    18

void        pasarBegin();
const char* pasarSession();            // "PASAR_S0007"
const char* pasarSessionDir();         // "/PASAR_S0007"
uint32_t    pasarSessionSeq();

/* Registers or refreshes a visitor. Pass the alias the browser remembers, or
   an empty string to be assigned one. Writes the authoritative alias back
   into out (always NUL-terminated) and returns the live visitor count. */
int  pasarTouch(const char* aliasIn, char* out, size_t outN);
int  pasarCount();                     // live visitors, stale ones excluded
void pasarShutter(uint32_t imgNum, const char* alias);
void pasarNoteWritten(uint32_t imgNum);

uint32_t pasarPhotoCount();            // photographs THIS session
uint32_t pasarLastImage();
const char* pasarLastAuthor();
uint32_t pasarUptimeMs();

/* Compact room state for the poll. Kept small deliberately: this is fetched
   by every visitor every couple of seconds. */
String pasarRoomJson(bool camOk, bool sdOk, const char* devName);

bool pasarNewSession();                // admin only
void pasarSanitizeAlias(const char* in, char* out, size_t outN);

#endif /* EXHIBITION_MODE */
