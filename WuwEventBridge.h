/* ── WuwEventBridge ───────────────────────────────────────────────────────
 * A one-way, fire-and-forget announcement of social events on the local
 * subnet, for the robotic work (XEN-T1) exhibited beside WUW-01.
 *
 * Deliberately the thinnest thing that could work:
 *   · UDP broadcast, no connection, no acknowledgement, no retry
 *   · nothing is ever received, so a hostile or absent peer cannot affect us
 *   · no video, no image data, no continuous stream -- social events only
 *   · rate limited, and identical states are not re-announced
 *   · if XEN-T1 is not on the network, every send is a no-op into the void
 *     and WUW-01 behaves exactly as if this file did not exist
 *
 * WUW-01 knows nothing about robot behaviour. It states what happened; what
 * that means is entirely XEN-T1's business. Keeping that separation is the
 * point of the module.
 *
 * Compile out by leaving WUW_EVENT_BRIDGE undefined.
 */
#pragma once
#ifdef WUW_EVENT_BRIDGE

#include <Arduino.h>

#ifndef WUW_EVENT_PORT
#define WUW_EVENT_PORT 4210
#endif

void wuwBridgeBegin();                        // after the AP is up
void wuwBridgeBoot();
void wuwBridgeClientCount(int n);             // sent only when n changes
void wuwBridgeJoin(const char* alias);
void wuwBridgeShutter(uint32_t img, const char* alias);
void wuwBridgePhotoSaved(uint32_t img);
void wuwBridgeNote(uint32_t img);
void wuwBridgeTick();                         // call from loop(); emits IDLE

#else   /* bridge compiled out -- every call vanishes */
#define wuwBridgeBegin()               ((void)0)
#define wuwBridgeBoot()                ((void)0)
#define wuwBridgeClientCount(n)        ((void)0)
#define wuwBridgeJoin(a)               ((void)0)
#define wuwBridgeShutter(i,a)          ((void)0)
#define wuwBridgePhotoSaved(i)         ((void)0)
#define wuwBridgeNote(i)               ((void)0)
#define wuwBridgeTick()                ((void)0)
#endif
