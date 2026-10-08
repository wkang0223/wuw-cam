/* ── WUW LINK ─────────────────────────────────────────────────────────────
 * Cameras finding each other on a network or nearby radio, and moving frames
 * over IP only when both devices share a network.
 *
 * THREAT MODEL, BECAUSE THIS ONE LISTENS
 * WuwEventBridge is one-way by construction: it holds no receive path, so a
 * hostile peer cannot reach it. This module cannot make that promise -- a
 * mesh has to listen -- so the guarantee is made narrower instead:
 *
 *   · FIRE is the only inbound packet that causes a physical action: this
 *     camera photographing its own subject. BWO packets only draw remote
 *     player positions; they cannot edit the world or change settings.
 *   · Nothing inbound writes a file, changes a setting, or names a path.
 *   · Files move only when THIS device decides to pull one. There is no
 *     push. A peer cannot put anything on our card.
 *   · FIRE obeys the same shutter mutex and minimum gap as the button, so a
 *     packet flood costs one photograph every SHUTTER_MIN_GAP, not a queue.
 *   · Packets carry a group token. It is a courtesy, not a secret: UDP on a
 *     shared LAN and ESP-NOW broadcasts are readable by nearby users. It
 *     separates two rigs in the same room; it is not authentication.
 *
 * Port 4211, deliberately not the bridge's 4210, so the bridge keeps its
 * "nothing is ever received" property whatever happens here.
 *
 * Compile out by leaving WUW_LINK undefined.
 */
#pragma once
#ifdef WUW_LINK

#include <Arduino.h>

#ifndef WUW_LINK_PORT
#define WUW_LINK_PORT 4211
#endif

#define LINK_MAX_PEERS   8
#define LINK_STALE_MS    9000UL     // three missed beacons and a peer is gone
#define LINK_BEACON_MS   2500UL

struct LinkGamePose {
  uint16_t x100, y100, angle1000;
  uint8_t level;
  uint8_t fragments;  // seven shared collection bits
};

struct LinkGamePeer {
  char id[9];
  LinkGamePose pose;
  uint32_t seen;
};

// Position-only multiplayer. State is a 10 Hz broadcast; no video is sent.
void linkGamePublish(const LinkGamePose& pose);
void linkGameStop();
size_t linkGamePeers(LinkGamePeer* out, size_t cap);

/* What we tell the others about ourselves. Filled in by main; the link task
   only reads it, so neither side has to lock. */
struct LinkSelf {
  const char* name;
  const char* session;
  uint32_t    photos;
  bool        sd;
  bool        cam;
};
void linkPublish(const LinkSelf& s);

/* The one thing a peer may ask of us. Same signature as the panel's, and it
   is wired to the same serialised shutter. */
typedef bool (*LinkShootFn)(const char* who);
void linkHooks(LinkShootFn shoot);

void linkBegin();
bool linkUp();
int  linkPeerCount();

/* Allocation-free: writes into the caller's buffer, returns bytes used.
   Called from a request handler at whatever rate a browser polls. */
size_t linkPeersJson(char* out, size_t cap);

/* Broadcasts a synchronised shutter and schedules our own for the same
   moment. leadMs is how long everyone waits, so slower peers still make it. */
bool linkFire(uint16_t leadMs, const char* tag);

/* Copies one frame off a peer onto our card. Runs on its own task -- an
   over-the-air pull of a 5 MP frame takes seconds and must never sit inside
   a request handler. One job at a time; a second call while busy is refused. */
bool        linkPullStart(const char* ip, int n, const char* peerName);
bool        linkPullBusy();
const char* linkPullStatus();

void linkSetGroup(const char* g);       // persisted
const char* linkGroup();

#else
#define linkPublish(s)          ((void)0)
#define linkHooks(f)            ((void)0)
#define linkBegin()             ((void)0)
#define linkUp()                (false)
#define linkPeerCount()         (0)
#define linkPeersJson(o,c)      ((size_t)0)
#define linkFire(l,t)           (false)
#define linkPullStart(i,n,p)    (false)
#define linkPullBusy()          (false)
#define linkPullStatus()        ("disabled")
#define linkSetGroup(g)         ((void)0)
#define linkGroup()             ("")
#endif
