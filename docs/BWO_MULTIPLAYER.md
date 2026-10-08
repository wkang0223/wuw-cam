# BWO multiplayer transport

The integrated `wuwcam` and `wuwcam-ov3660` builds broadcast WUW LINK
discovery and BWO game state on both transports:

- ESP-NOW from the camera's SoftAP, for nearby units without a router.
- UDP port 4211, for units on the same local IP network (including a unit
  joined to another camera's AP).

Both cameras must run a multiplayer build and use the same link group. The
default group is `wuw`. Open BWO on each panel. A peer appears in the rig page
and as a depth-tested marker in the BWO view. The pips at the top of the game
view count actively playing peers. Collected fragments converge across active
players. If a peer stops sending, its game marker disappears after 1.5 s;
its discovery entry expires after 9 s.

Only quantized position, direction, level, and seven fragment bits are sent,
at 10 Hz. The panel still renders locally. Walls, photo textures, enemies,
and saved patches remain local to each camera; this is not yet a shared-world
editor. This radio path does not send camera frames or live video. The existing
MJPEG web stream remains the video path. If a peer is discovered only by
ESP-NOW, its IP is unknown, so the rig page does not offer gallery pull or a
web link. Those operations require a shared Wi-Fi network.
Collective shutter commands also remain on the shared Wi-Fi path; a nearby
radio beacon alone cannot trigger another camera's shutter.

ESP-NOW uses the current Wi-Fi channel. With no upstream station, WUW cameras
start their APs on channel 1. Joining an external Wi-Fi network can move the
AP to that network's channel; nearby cameras on different channels cannot
hear one another over ESP-NOW. For a reliable exhibition setup, put every
camera on one 2.4 GHz venue network or leave all cameras in AP-only mode.
Radio broadcasts are unencrypted, and the group string is not a password.

Do not infer measured latency or range from a successful build: test with at
least two powered cameras in the actual space, while preview and recording are
active. Check that peer markers appear, movement stays smooth, fragment
collection converges, and a powered-off peer disappears without freezing the
panel. The current build has not been flashed or measured on a multi-camera
rig yet.
