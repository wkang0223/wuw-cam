#!/usr/bin/env python3
"""
WUW CAM — Y3K two-part housing for the ESP32-S3 + OV5640 stack.

All coordinates are in the SAME frame as Camera.stl, so the case and the board
can be loaded together and checked for fit without moving anything.

Board facts measured from Camera.stl (not assumed):
  assembly bbox   X[-21.90, 29.70]  Y[-49.92, 32.91]  Z[-28.81, 4.34]
  ESP32-S3 board  X[ -5.73, 22.47]  Y[-35.10, 16.16]
  camera module   8.50 x 8.50 x 5.00  centred (8.24,  4.32) firing -Z
  SD holder      15.80 x 17.60 x 2.30 centred (8.27,  6.38), card enters +Y
  USB-C x2        8.94 x 7.32 x 4.26  centred (2.74/14.04, -31.52), flush -Y edge

Screen faces +Z (front shell), camera faces -Z (back shell). No antenna.
"""
from build123d import *

# ── board envelope ─────────────────────────────────────────────────────────
BX0, BX1 = -21.90, 29.70
BY0, BY1 = -49.92, 32.91
BZ0, BZ1 = -28.81,  4.34

CLR   = 0.7      # air gap around the board stack
WALL  = 3.0      # shell thickness — 3 mm so it survives being dropped
CORNER = 19.0    # silhouette roundness — the Y3K pebble
SPLIT_Z = -6.0   # everything below drops into the back shell

# The board fills the whole -Y half of the cavity: an occupancy map over the
# post z-range found ZERO clear sites in either -Y quadrant. Rather than put
# screws where they would foul the carrier plate, the shell grows a chin at
# -Y. It houses the two bottom bosses, gives USB-C overmoulds room to enter,
# and reads like the chin on the reference handset.
CHIN = 9.0
IX0, IX1 = BX0 - CLR, BX1 + CLR
IY0, IY1 = BY0 - CLR - CHIN, BY1 + CLR
IZ0, IZ1 = BZ0 - CLR, BZ1 + CLR
IW, IL, IH = IX1 - IX0, IY1 - IY0, IZ1 - IZ0

OW, OL, OH = IW + 2*WALL, IL + 2*WALL, IH + 2*WALL
CX, CY, CZ = (IX0+IX1)/2, (IY0+IY1)/2, (IZ0+IZ1)/2
OZ0, OZ1 = CZ - OH/2, CZ + OH/2

# ── component anchors ──────────────────────────────────────────────────────
CAM_X, CAM_Y = 8.24, 4.32
SD_X,  SD_Y  = 8.27, 6.38
USB_Y, USB_Z = -31.52, -21.74
USB_X = (2.74, 14.04)

def rrect(w, l, r, h, cx=CX, cy=CY, cz=CZ):
    """Rounded-corner slab centred on (cx,cy,cz)."""
    b = Pos(cx, cy, cz) * Box(w, l, h)
    return fillet(b.edges().filter_by(Axis.Z), radius=r)



def build(openings=True, emoji=True):
    """openings=False -> blank shell (no USB/SD ports, no wuw relief)."""
    # ── 1. outer body: pebble with a bulged back ───────────────────────────────
    outer = rrect(OW, OL, CORNER, OH)
    # soften the front/back faces so it reads as moulded, not milled
    outer = fillet(outer.edges().group_by(Axis.Z)[0], radius=2.6)
    outer = fillet(outer.edges().group_by(Axis.Z)[-1], radius=2.6)

    # a shallow lens-shaped swell on the back, the way the reference bulges
    bulge = Pos(CX, CY, OZ0 + 9.0) * scale(Sphere(radius=1), by=(OW*0.44, OL*0.40, 11.0))
    outer = outer + bulge
    outer = outer & rrect(OW, OL, CORNER, OH + 40)   # keep the silhouette honest

    # ── 2. cavity ──────────────────────────────────────────────────────────────
    # A 16 mm cavity radius clipped the board's square corners (checked: corner
    # at (25.8,41.4) sat 21.6 mm from the arc centre, outside a 16 mm arc). Keep
    # the cavity nearly square so the PCB actually drops in.
    cavity = rrect(IW, IL, 2.5, IH)
    shell = outer - cavity

    # ── 3. split into two shells ───────────────────────────────────────────────
    big = 400
    front_half = Pos(CX, CY, SPLIT_Z + big/2) * Box(big, big, big)
    back_half  = Pos(CX, CY, SPLIT_Z - big/2) * Box(big, big, big)
    front = shell & front_half
    back  = shell & back_half

    # tongue on the back shell / groove in the front, so the seam self-aligns
    T_H, T_W = 2.0, WALL/2
    tongue_out = rrect(IW + T_W, IL + T_W, 2.5, T_H,
                       cz=SPLIT_Z + T_H/2)
    tongue_in  = rrect(IW - T_W, IL - T_W, 2.5, T_H + 0.2,
                       cz=SPLIT_Z + T_H/2)
    tongue = (tongue_out - tongue_in) & outer
    back = back + tongue
    front = front - rrect(IW + T_W + 0.25, IL + T_W + 0.25,
                          2.5, T_H + 0.15, cz=SPLIT_Z + T_H/2 - 0.05) \
                  + (front & rrect(IW - T_W - 0.25, IL - T_W - 0.25,
                                   2.5, T_H, cz=SPLIT_Z + T_H/2))

    # ── 4. M2 screw posts, four corners ────────────────────────────────────────
    POST_R, PILOT_R, HEAD_R = 3.0, 0.85, 2.1
    # Sites verified clear of board material over the post's whole z-range.
    POST_XY = [(-17.6,  30.0), (25.4,  30.0),     # +Y: 10 mm clearance measured
               (-17.6, -55.4), (25.4, -55.4)]     # -Y: inside the new chin
    posts, holes, heads = None, None, None
    for (x, y) in POST_XY:
        if True:
            h = SPLIT_Z - (OZ0 + WALL)
            p = Pos(x, y, OZ0 + WALL + h/2) * Cylinder(POST_R, h)
            d = Pos(x, y, OZ0 + WALL + h/2) * Cylinder(PILOT_R, h + 1)
            c = Pos(x, y, SPLIT_Z + (OZ1-SPLIT_Z)/2) * Cylinder(HEAD_R*0.52, OZ1-SPLIT_Z+2)
            posts = p if posts is None else posts + p
            holes = d if holes is None else holes + d
            heads = c if heads is None else heads + c
    back = back + posts - holes
    front = front - heads
    # countersink the screw heads on the outer front face
    for (x, y) in POST_XY:
        front = front - (Pos(x, y, OZ1 - 1.0) * Cylinder(HEAD_R, 2.4))

    # ── 5. openings ────────────────────────────────────────────────────────────
    # screen window (front) — the display plate is 42.75 x 60.82 at Z ~ +3
    # The display plate is 42.75 x 60.82 centred (4.60, -3.62). Centring the
    # window on the CASE instead put it 3.9 mm out in Y, which would have shown
    # bezel on one side and clipped the glass on the other.
    DSP_X, DSP_Y = 4.60, -3.62
    SCR_W, SCR_L = 39.8, 57.8            # 1.5 mm lip all round retains the glass
    front = front - rrect(SCR_W, SCR_L, 4.0, 2*WALL + 8, cx=DSP_X, cy=DSP_Y, cz=OZ1)
    # recess so the glass sits under the lip rather than poking through
    front = front - rrect(43.6, 61.7, 5.0, 2.4, cx=DSP_X, cy=DSP_Y,
                          cz=OZ1 - WALL - 1.2)

    # camera port (back) with a shallow lens recess — always present
    back = back - (Pos(CAM_X, CAM_Y, OZ0) * Cylinder(4.6, 2*WALL + 8))
    back = back - (Pos(CAM_X, CAM_Y, OZ0 + 0.9) * Cylinder(7.4, 1.8))

    # microSD slot on the +Y edge, aligned to the holder
    if openings:
        back = back - (Pos(SD_X, IY1 + WALL, -22.6) * Box(16.6, 2*WALL + 6, 3.4))

    # two USB-C ports on the -Y edge
    # The ports sit ~15 mm inboard of the chin wall, so a cable enters through a
    # short tunnel; the wall opening is sized for the overmould, not the plug.
    if openings:
        for ux in USB_X:
            back = back - (Pos(ux, IY0 - WALL, USB_Z) * Box(13.0, 2*WALL + 8, 8.4))

    # ── 5b. shutter button seat, in the chin ──────────────────────────────
    # The board fills the cavity to within 0.7 mm everywhere except the chin,
    # so this is the only interior volume a 12x12 tactile switch can occupy.
    # It also lands where the reference handset puts its buttons: front face,
    # below the screen, under a thumb.
    BTN_X, BTN_Y = 4.60, -55.0
    front = front - (Pos(BTN_X, BTN_Y, OZ1) * Cylinder(2.2, 2*WALL + 8))
    # four corner posts hold the switch body 3.2 mm back, so the stem just
    # clears the outer face rather than standing proud of it
    for dx, dy in ((-6.2,-6.2), (6.2,-6.2), (-6.2,6.2), (6.2,6.2)):
        front = front + (Pos(BTN_X+dx, BTN_Y+dy, OZ1 - WALL - 1.6) *
                         Cylinder(1.1, 3.2))

    # ── 6. Y3K rib detail: elongated lozenge recesses down both flanks ─────────
    DENT = 1.5                        # into a 3.0 mm wall — leaves 1.5 mm
    ribs = None
    for sx in (-1, 1):
        for yy, ll in [(-46, 12), (-30, 16), (-13, 17), (5, 15), (22, 11)]:
            # centred ON the surface, so the ellipsoid's own semi-axis is the depth
            r = Pos(CX + sx*(OW/2), yy, CZ + 1.0) * \
                scale(Sphere(radius=1), by=(DENT, ll/2, 4.2))
            ribs = r if ribs is None else ribs + r
    back = back - ribs
    front = front - ribs

    # ── 6b. ribcage spine on the back, the signature of the reference ─────────
    # The back is bulged here, so a rib centred on OZ0 would sit *inside* the
    # swell. Build them tall, then trim: cavity side so they cannot foul the
    # board, top side so nothing floats. What is left stands proud of the bulge.
    spine = None
    for i in range(6):
        yy = -34.0 + i*5.6
        r = Pos(CAM_X, yy, OZ0) * scale(Sphere(radius=1),
                                             by=(13.0 - i*0.7, 2.1, 3.6))
        spine = r if spine is None else spine + r
    spine = spine - Pos(CX, CY, OZ0 + 40) * Box(200, 200, 80)
    spine = spine - cavity
    back = back + spine

    # ── 7. the wuw face, embossed on the back above the camera ────────────────
    # The back surface is flat at Z=OZ0 this far up-board (the bulge has fallen
    # away by here), so a flat relief sits down properly with no floating edges.
    FACE_Y = 21.0                       # well clear of the camera port at Y=4.32
    RELIEF = 1.0                        # proud of the surface
    face = None
    for ex in (-7.0, 7.0):                                   # eyes
        e = Pos(CAM_X + ex, FACE_Y + 4.0, OZ0 - RELIEF/2 + 0.25) * \
            scale(Sphere(radius=1), by=(3.0, 3.9, RELIEF + 0.5))
        face = e if face is None else face + e
    mouth = Pos(CAM_X, FACE_Y - 5.2, OZ0 - RELIEF) * \
            extrude(Text("w", font_size=13.0,
                         align=(Align.CENTER, Align.CENTER)), RELIEF + 0.5)
    face = face + mouth
    # clip to a true relief: nothing sunk below the surface, nothing floating
    face = face - Pos(CX, CY, OZ0 + 40) * Box(200, 200, 80)
    if emoji:
        back = back + face


    return back, front


# ── decorative language from the reference: cell columns + bead chains ────
def decorate(back, front):
    # two columns of oval cells either side of the spine, as on the reference
    cells = None
    for sx in (-1, 1):
        for yy, w, h in [(-44, 9.0, 6.0), (-33, 9.5, 7.0), (-21, 10.0, 7.5),
                         (-9, 10.0, 7.5), (3, 9.5, 7.0), (15, 8.5, 6.0)]:
            c = Pos(CAM_X + sx*19.0, yy, OZ0) * scale(
                Sphere(radius=1), by=(w/2, h/2, 1.3))
            cells = c if cells is None else cells + c
    back = back - cells

    # bead chains running down both side edges — the reference's signature
    beads = None
    for sx in (-1, 1):
        for i in range(17):
            yy = -52.0 + i*5.2
            r = 1.55 + 0.35*np.sin(i*0.55)
            bd = Pos(CX + sx*(OW/2 - 0.5), yy, CZ + 3.0) * Sphere(radius=r)
            beads = bd if beads is None else beads + bd
    beads_back  = beads - Pos(CX, CY, SPLIT_Z + 200) * Box(400, 400, 400)
    beads_front = beads - Pos(CX, CY, SPLIT_Z - 200) * Box(400, 400, 400)
    back  = back  + beads_back
    front = front + beads_front

    # raised organic bezel around the glass
    bez = rrect(48.6, 66.6, 7.0, 2.6, cx=DSP_X, cy=DSP_Y, cz=OZ1) \
        - rrect(41.4, 59.4, 4.6, 6.0, cx=DSP_X, cy=DSP_Y, cz=OZ1)
    bez = bez - Pos(CX, CY, OZ1 - 200) * Box(400, 400, 400)
    front = front + bez

    # chin detail: the speaker-slot oval below the screen
    front = front - (Pos(DSP_X, IY0 + 6.5, OZ1) * scale(
        Sphere(radius=1), by=(7.0, 2.6, 1.6)))
    return back, front


import numpy as np
DSP_X, DSP_Y = 4.60, -3.62

# ── export both variants ──────────────────────────────────────────────────
full_b, full_f = build(openings=True, emoji=True)
full_b, full_f = decorate(full_b, full_f)
export_stl(full_b, "wuwcam_case_back.stl")
export_stl(full_f, "wuwcam_case_front.stl")
export_step(full_b + Pos(0, 0, 70)*full_f, "wuwcam_case.step")

blank_b, blank_f = build(openings=False, emoji=False)
blank_b, blank_f = decorate(blank_b, blank_f)
export_stl(blank_b, "wuwcam_case_back_BLANK.stl")
export_stl(blank_f, "wuwcam_case_front_BLANK.stl")

for name, part in (("full  back", full_b), ("full  front", full_f),
                   ("blank back", blank_b), ("blank front", blank_f)):
    bb = part.bounding_box()
    print(f"{name:12s} {bb.max.X-bb.min.X:6.2f} x {bb.max.Y-bb.min.Y:6.2f} x "
          f"{bb.max.Z-bb.min.Z:6.2f} mm   vol {part.volume/1000:6.2f} cm3")
print(f"outer  {OW:.2f} x {OL:.2f} x {OH:.2f} mm   wall {WALL} mm")
