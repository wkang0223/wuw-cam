"""
WUW CAM housing — organic pass, run headless in Blender.

  "/Applications/Blender 3.app/Contents/MacOS/Blender" --background \
      --python blender_sculpt.py

Approach. Metaballs were tried first and abandoned: their field size is hard
to predict, the blob came out 356 mm across and swallowed the form. Instead
the outer surface is a base prism, voxel-remeshed to uniform topology, with
every vertex displaced along its normal by an explicit field. That field
carries the reference's vocabulary — ribcage ridges, cell recesses, bead
chains, a camera brow — and because the magnitudes are explicit, the wall
thickness is bounded by construction.

Wall budget: the base wall is BASE_WALL (4.0 mm) and the deepest inward
displacement is DENT_MAX (1.5 mm), so the thinnest wall anywhere is 2.5 mm.
The verified cavity and every opening are subtracted AFTER sculpting, so the
board fit proved against the build123d model is preserved exactly.
"""
import bpy, bmesh, math
from mathutils import Vector

# ── parameters, identical to the build123d model ──────────────────────────
CLR, CHIN, SPLIT_Z = 0.7, 9.0, -6.0
BASE_WALL = 4.0          # so a 1.5 mm dent still leaves 2.5 mm
DENT_MAX  = 1.5
BX0, BX1 = -21.90, 29.70
BY0, BY1 = -49.92, 32.91
BZ0, BZ1 = -28.81,  4.34
IX0, IX1 = BX0-CLR, BX1+CLR
IY0, IY1 = BY0-CLR-CHIN, BY1+CLR
IZ0, IZ1 = BZ0-CLR, BZ1+CLR
IW, IL, IH = IX1-IX0, IY1-IY0, IZ1-IZ0
OW, OL, OH = IW+2*BASE_WALL, IL+2*BASE_WALL, IH+2*BASE_WALL
CX, CY, CZ = (IX0+IX1)/2, (IY0+IY1)/2, (IZ0+IZ1)/2
OZ0, OZ1 = CZ-OH/2, CZ+OH/2
CAM_X, CAM_Y = 8.24, 4.32
SD_X = 8.27
USB_X, USB_Z = (2.74, 14.04), -21.74
DSP_X, DSP_Y = 4.60, -3.62
POST_XY = [(-17.6, 30.0), (25.4, 30.0), (-17.6, -55.4), (25.4, -55.4)]
VOXEL = 0.7

import sys
_a = sys.argv[sys.argv.index("--")+1:] if "--" in sys.argv else []
VARIANT  = _a[0] if _a else "full"
OPENINGS = (VARIANT == "full")
EMOJI    = (VARIANT == "full")
SUFFIX   = "" if VARIANT == "full" else "_BLANK"

def log(t): print("  [%s]" % t)

def clear():
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)

def rbox(name, w, l, h, cx, cy, cz, r, seg=16):
    """Prism with a rounded-rectangle plan and flat top/bottom."""
    r = max(0.0, min(r, min(w, l)/2 - 1e-6))
    if r <= 1e-6:
        pts = [(-w/2,-l/2),(w/2,-l/2),(w/2,l/2),(-w/2,l/2)]
    else:
        hw, hl = w/2-r, l/2-r
        pts = []
        for ox, oy, a0 in ((hw,-hl,-math.pi/2),(hw,hl,0.0),
                           (-hw,hl,math.pi/2),(-hw,-hl,math.pi)):
            for i in range(seg+1):
                a = a0 + i*(math.pi/2)/seg
                pts.append((ox+r*math.cos(a), oy+r*math.sin(a)))
        ded=[pts[0]]
        for q in pts[1:]:
            if (q[0]-ded[-1][0])**2+(q[1]-ded[-1][1])**2 > 1e-10: ded.append(q)
        pts=ded
    me = bpy.data.meshes.new(name); bm = bmesh.new()
    vs = [bm.verts.new((x,y,-h/2)) for x,y in pts]
    bm.faces.new(vs); bm.normal_update()
    res = bmesh.ops.extrude_face_region(bm, geom=bm.faces[:])
    nv = [e for e in res["geom"] if isinstance(e, bmesh.types.BMVert)]
    bmesh.ops.translate(bm, verts=nv, vec=(0,0,h))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    bm.to_mesh(me); bm.free()
    o = bpy.data.objects.new(name, me); bpy.context.collection.objects.link(o)
    o.location = (cx,cy,cz)
    # transform_apply acts on SELECTED objects. Setting only the active object
    # left the location unapplied, so object-local coords stayed offset from
    # world -- booleans still worked (they use world matrices) but the
    # hand-written displacement field silently evaluated in the wrong frame.
    bpy.ops.object.select_all(action='DESELECT')
    o.select_set(True)
    bpy.context.view_layer.objects.active = o
    bpy.ops.object.transform_apply(location=True, rotation=False, scale=False)
    return o

def cyl(name, r, h, cx, cy, cz):
    bpy.ops.mesh.primitive_cylinder_add(radius=r, depth=h,
                                        location=(cx,cy,cz), vertices=64)
    o = bpy.context.object; o.name = name
    return o

def boolean(target, cutter, op):
    bpy.context.view_layer.objects.active = target
    m = target.modifiers.new("b", 'BOOLEAN')
    m.operation = op; m.object = cutter; m.solver = 'EXACT'
    bpy.ops.object.modifier_apply(modifier=m.name)
    bpy.data.objects.remove(cutter, do_unlink=True)

def dims(o, tag):
    bb=[o.matrix_world @ Vector(c) for c in o.bound_box]
    xs=[v.x for v in bb]; ys=[v.y for v in bb]; zs=[v.z for v in bb]
    print(f"  [{tag}] {max(xs)-min(xs):6.2f} x {max(ys)-min(ys):6.2f} x "
          f"{max(zs)-min(zs):6.2f}  faces {len(o.data.polygons)}")

# ── smooth helpers for the displacement field ────────────────────────────
def sstep(a, b, x):
    if b == a: return 0.0
    t = min(1.0, max(0.0, (x-a)/(b-a)))
    return t*t*(3-2*t)

def seg_d(px, py, ax, ay, bx, by):
    vx, vy = bx-ax, by-ay
    wx, wy = px-ax, py-ay
    dd = vx*vx + vy*vy
    t = 0.0 if dd < 1e-9 else max(0.0, min(1.0, (wx*vx + wy*vy)/dd))
    return math.hypot(px-(ax+t*vx), py-(ay+t*vy))

def bump(d, w):
    """1 at d=0, falling to 0 at |d|=w, smooth."""
    return 1.0 - sstep(0.0, w, abs(d))

clear()

# ── 1. base prism, remeshed to uniform topology ─────────────────────────
base = rbox("shellbase", OW, OL, OH, CX, CY, CZ, 19.0)
m = base.modifiers.new("rm", 'REMESH')
m.mode = 'VOXEL'; m.voxel_size = VOXEL; m.use_smooth_shade = False
bpy.context.view_layer.objects.active = base
bpy.ops.object.modifier_apply(modifier=m.name)
dims(base, "remeshed base")
assert base.matrix_world.translation.length < 1e-6, \
    "base transform not applied -- displacement field would use the wrong frame"

# ── 2. displace along normals by the reference's vocabulary ─────────────
me = base.data
me.calc_normals_split() if hasattr(me, "calc_normals_split") else None
bm = bmesh.new(); bm.from_mesh(me); bm.verts.ensure_lookup_table()
bm.normal_update()

RIB_Y  = [-36.0 + i*6.0 for i in range(7)]          # spine vertebrae
CELL_Y = [-45, -34, -22, -10, 2, 14]                 # flanking cells
BEAD_Y = [IY0 + 5.0 + i*(IL-10.0)/18.0 for i in range(19)]

for v in bm.verts:
    p = v.co; n = v.normal
    if n.length < 1e-6: continue
    n = n.normalized()
    backness = sstep(0.25, 0.85, -n.z)      # faces pointing out the back
    sideness = sstep(0.45, 0.92, abs(n.x))  # faces pointing out the flanks
    frontness = sstep(0.25, 0.85, n.z)
    d = 0.0

    # ribcage: transverse ridges on the back, tapering toward the ends
    if backness > 0.01:
        for i, ry in enumerate(RIB_Y):
            halfw = 12.0 - abs(i-3)*0.9
            if abs(p.x - CAM_X) < halfw and abs(p.y - ry) < 3.0:
                across = bump(p.x - CAM_X, halfw)
                along  = bump(p.y - ry, 3.0)
                d += 2.3 * backness * across * along

    # cell columns: shallow scooped ovals either side of the spine
    if backness > 0.01:
        for sx in (-1, 1):
            cx_ = CAM_X + sx*18.5
            for cy_ in CELL_Y:
                ex, ey = (p.x-cx_)/7.0, (p.y-cy_)/5.5
                q = math.hypot(ex, ey)
                if q < 1.0:
                    d -= DENT_MAX * backness * bump(q, 1.0)

    # bead chains running down both flanks
    if sideness > 0.01:
        for i, by in enumerate(BEAD_Y):
            rr = 2.6 + 0.6*math.sin(i*0.5)
            dy, dz = p.y - by, p.z - (CZ + 3.0)
            q = math.hypot(dy/rr, dz/(rr*1.05))
            if q < 1.0:
                d += 2.0 * sideness * bump(q, 1.0)

    # camera brow: a raised pod around the lens, as on the reference
    if backness > 0.01:
        q = math.hypot((p.x-CAM_X)/13.0, (p.y-CAM_Y)/10.0)
        if q < 1.0:
            d += 1.7 * backness * bump(q, 1.0)

    # the wuw face: two eyes and a w mouth, up-board and clear of the brow
    if EMOJI and backness > 0.01:
        for ex in (-7.0, 7.0):
            q = math.hypot((p.x-(CAM_X+ex))/3.0, (p.y-28.0)/3.9)
            if q < 1.0:
                d += 1.6 * backness * bump(q, 1.0)
        W = [(-7.0, 3.5), (-3.5, -3.5), (0.0, 2.0), (3.5, -3.5), (7.0, 3.5)]
        md = 1e9
        for i in range(len(W)-1):
            md = min(md, seg_d(p.x-CAM_X, p.y-20.0,
                               W[i][0], W[i][1], W[i+1][0], W[i+1][1]))
        if md < 1.7:
            d += 1.5 * backness * bump(md, 1.7)

    # screen brow on the front top edge
    if frontness > 0.01:
        q = math.hypot((p.x-DSP_X)/22.0, (p.y-(DSP_Y+34.0))/7.0)
        if q < 1.0:
            d += 1.4 * frontness * bump(q, 1.0)

    v.co = p + n*d

bm.to_mesh(me); bm.free()
me.update()
dims(base, "displaced")

sm = base.modifiers.new("sm", 'SMOOTH')
sm.factor = 0.45; sm.iterations = 2
bpy.context.view_layer.objects.active = base
bpy.ops.object.modifier_apply(modifier=sm.name)

# Displacing along normals lets neighbouring features (ribs meeting the brow,
# beads on a curved flank) push through each other. Those self-intersections
# survive as invalid geometry and made the EXACT boolean solver collapse the
# blank variant to 594 faces. A second voxel remesh rebuilds a clean manifold
# so every downstream boolean gets valid input.
rm2 = base.modifiers.new("rm2", 'REMESH')
rm2.mode = 'VOXEL'; rm2.voxel_size = 0.6; rm2.use_smooth_shade = False
bpy.ops.object.modifier_apply(modifier=rm2.name)
dims(base, "cleanup remesh")

# ── 3. subtract the verified cavity, then the openings ──────────────────
organic = base
boolean(organic, rbox("cav", IW, IL, IH, CX, CY, CZ, 2.5), 'DIFFERENCE')
dims(organic, "cavity cut")
boolean(organic, cyl("cam", 4.6, 60, CAM_X, CAM_Y, OZ0-20), 'DIFFERENCE')
boolean(organic, cyl("lens", 7.4, 2.6, CAM_X, CAM_Y, OZ0+0.6), 'DIFFERENCE')
if OPENINGS:
    boolean(organic, rbox("sd", 16.6, 2*BASE_WALL+12, 3.4, SD_X,
                          IY1+BASE_WALL, -22.6, 0), 'DIFFERENCE')
    for i, ux in enumerate(USB_X):
        boolean(organic, rbox(f"usb{i}", 13.0, 2*BASE_WALL+14, 8.4, ux,
                              IY0-BASE_WALL, USB_Z, 0), 'DIFFERENCE')
boolean(organic, rbox("scr", 39.8, 57.8, 2*BASE_WALL+16, DSP_X, DSP_Y, OZ1, 4.0),
        'DIFFERENCE')
boolean(organic, rbox("scrrec", 43.6, 61.7, 2.6, DSP_X, DSP_Y,
                      OZ1-BASE_WALL-1.3, 5.0), 'DIFFERENCE')
dims(organic, "openings cut")

# ── 3b. shutter button seat, in the chin ──────────────────────────────
# Only interior volume not taken by the board. Front face, below the screen.
BTN_X, BTN_Y = 4.60, -55.0
boolean(organic, cyl("btnhole", 2.2, 2*BASE_WALL + 16, BTN_X, BTN_Y, OZ1),
        'DIFFERENCE')

# ── 4. split and add posts ─────────────────────────────────────────────
def half(src, name, upper):
    o = src.copy(); o.data = src.data.copy(); o.name = name
    bpy.context.collection.objects.link(o)
    z = SPLIT_Z + (200 if upper else -200)
    boolean(o, rbox(name+"k", 500, 500, 400, CX, CY, z, 0), 'INTERSECT')
    return o

front = half(organic, "wuw_front", True);  dims(front, "front half")
back  = half(organic, "wuw_back",  False); dims(back,  "back half")
bpy.data.objects.remove(organic, do_unlink=True)

# switch seat posts, added to the front shell after the split
for i, (dx, dy) in enumerate(((-6.2,-6.2), (6.2,-6.2), (-6.2,6.2), (6.2,6.2))):
    boolean(front, cyl(f"seat{i}", 1.1, 3.2, BTN_X+dx, BTN_Y+dy,
                       OZ1 - BASE_WALL - 1.6), 'UNION')

ph = SPLIT_Z - (OZ0 + BASE_WALL)
for i, (x, y) in enumerate(POST_XY):
    boolean(back, cyl(f"p{i}", 3.2, ph, x, y, OZ0+BASE_WALL+ph/2), 'UNION')
for i, (x, y) in enumerate(POST_XY):
    boolean(back, cyl(f"ph{i}", 0.85, ph+3, x, y, OZ0+BASE_WALL+ph/2), 'DIFFERENCE')
    boolean(front, cyl(f"fh{i}", 1.15, OZ1-SPLIT_Z+8, x, y,
                       SPLIT_Z+(OZ1-SPLIT_Z)/2), 'DIFFERENCE')
    boolean(front, cyl(f"cs{i}", 2.1, 2.6, x, y, OZ1-1.1), 'DIFFERENCE')

# ── 5. export ──────────────────────────────────────────────────────────
import os
out = os.path.dirname(os.path.abspath(__file__))
for obj, fn in ((back, f"wuwcam_organic_back{SUFFIX}.stl"),
                (front, f"wuwcam_organic_front{SUFFIX}.stl")):
    bpy.ops.object.select_all(action='DESELECT')
    obj.select_set(True); bpy.context.view_layer.objects.active = obj
    bpy.ops.wm.stl_export(filepath=os.path.join(out, fn),
                          export_selected_objects=True)
    dims(obj, "EXPORT " + fn)
print(f"  [envelope] {OW:.2f} x {OL:.2f} x {OH:.2f}  base wall {BASE_WALL} "
      f"min wall {BASE_WALL-DENT_MAX}")
print("DONE")
