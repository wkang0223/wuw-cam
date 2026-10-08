#!/usr/bin/env python3
"""Assert no two subsystems claim the same GPIO, and that none is forbidden.

Pin conflicts do not fail the build. They fail as a camera that will not
initialise, or a display that works until the SD card is touched -- symptoms
that look like anything except what they are. This reads the pin numbers out
of the real source and checks them.

    python3 tools/pincheck.py
"""
import re, sys, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent

# ESP32-S3-WROOM N16R8: what the silicon and the module take before we start.
FORBIDDEN = {}
for p in range(26, 33): FORBIDDEN[p] = "SPI flash"
for p in range(33, 38): FORBIDDEN[p] = "octal PSRAM (the R8 trap)"
FORBIDDEN[43] = FORBIDDEN[44] = "UART0 console"
for p in (22, 23, 24, 25): FORBIDDEN[p] = "does not exist on the S3"
STRAPPING = {0: "boot mode", 3: "JTAG select", 45: "VDD_SPI", 46: "boot log"}

def defs(path, names):
    txt = (ROOT / path).read_text()
    out = {}
    for n in names:
        m = re.search(r"^#define\s+%s\s+(-?\d+)" % re.escape(n), txt, re.M)
        if m:
            v = int(m.group(1))
            if v >= 0:
                out[n] = v
    return out

groups = {
  "camera": defs("main_s3.cpp", [
      "XCLK_GPIO_NUM","SIOD_GPIO_NUM","SIOC_GPIO_NUM","Y9_GPIO_NUM","Y8_GPIO_NUM",
      "Y7_GPIO_NUM","Y6_GPIO_NUM","Y5_GPIO_NUM","Y4_GPIO_NUM","Y3_GPIO_NUM",
      "Y2_GPIO_NUM","VSYNC_GPIO_NUM","HREF_GPIO_NUM","PCLK_GPIO_NUM",
      "PWDN_GPIO_NUM","RESET_GPIO_NUM"]),
  "microSD": defs("main_s3.cpp", ["SD_CLK_PIN","SD_CMD_PIN","SD_D0_PIN"]),
  "button":  defs("main_s3.cpp", ["BTN_PIN"]),
  "panel":   defs("panel.cpp",   ["LCD_SCLK","LCD_MOSI","LCD_MISO","LCD_CS",
                                  "LCD_DC","TCH_CS","LCD_BL","LCD_RST"]),
}

# lcdtest.cpp is the bring-up bench for the SAME wiring. If it drifts from
# panel.cpp, the bench proves a pin map nothing ships with.
bench = defs("lcdtest.cpp", ["LCD_SCLK","LCD_MOSI","LCD_MISO","LCD_CS",
                             "LCD_DC","TCH_CS","LCD_BL","LCD_RST"])
drift = [n for n in groups["panel"] if bench.get(n) != groups["panel"][n]]

owner, fails = {}, []
for n in drift:
    fails.append("lcdtest.cpp %s = %s but panel.cpp %s = %d"
                 % (n, bench.get(n), n, groups["panel"][n]))
for g, d in groups.items():
    for name, pin in sorted(d.items(), key=lambda kv: kv[1]):
        if pin in owner:
            fails.append("GPIO %d: %s (%s) collides with %s (%s)"
                         % (pin, name, g, owner[pin][0], owner[pin][1]))
        else:
            owner[pin] = (name, g)
        if pin in FORBIDDEN:
            fails.append("GPIO %d: %s (%s) is claimed by %s"
                         % (pin, name, g, FORBIDDEN[pin]))

print("%-8s %s" % ("GPIO", "signal"))
for pin in sorted(owner):
    name, g = owner[pin]
    warn = "  <-- STRAPPING PIN (%s)" % STRAPPING[pin] if pin in STRAPPING else ""
    print("  %-6d %-18s %s%s" % (pin, name, g, warn))

free = [p for p in list(range(0, 22)) + list(range(38, 49))
        if p not in owner and p not in FORBIDDEN]
print("\nunclaimed: %s" % (", ".join(str(p) for p in free) or "none"))

# ── Physical header order ────────────────────────────────────────────────
# GPIO NUMBER ORDER IS NOT HEADER ORDER on this board, so contiguity has to be
# measured against the real silkscreen. This map is DERIVED, not measured: it
# comes from the Duck Life build's WIRING.md, which documents the same Freenove
# N16R8 module and states that 1/2/42/41 are "four in a row" and 21/47/48 are
# "three in a row" on the right header, with 14 on the left header below 13.
# Confirm it against the board before trusting the run count below.
HEADER = {
  "left":  [4, 5, 6, 7, 15, 16, 17, 18, 8, 3, 46, 9, 10, 11, 12, 13, 14],
  "right": [43, 44, 1, 2, 42, 41, 40, 39, 38, 37, 36, 35, 0, 45, 48, 47, 21],
}
POS = {p: (side, i) for side, pins in HEADER.items() for i, p in enumerate(pins)}

pp = groups["panel"].values()
placed = [p for p in pp if p in POS]
if len(placed) != len(list(pp)):
    print("\nheader map does not cover every panel pin - run count skipped")
else:
    runs, cur = [], None
    for p in sorted(placed, key=lambda x: POS[x]):
        if cur and POS[cur[-1]][0] == POS[p][0] and POS[p][1] == POS[cur[-1]][1] + 1:
            cur.append(p)
        else:
            if cur: runs.append(cur)
            cur = [p]
    runs.append(cur)
    inv = {v: k for k, v in groups["panel"].items()}
    print("\ndisplay harness = %d ribbon(s) on the header (map is derived, verify it):"
          % len(runs))
    for r in runs:
        print("  %s: %s" % (POS[r[0]][0],
              "  ".join("%d=%s" % (p, inv[p].replace("LCD_", "").replace("TCH_CS", "T_CS"))
                        for p in r)))

# ── Rust alternative firmware ────────────────────────────────────────────
# rust/wuw-board is a second copy of this map. It must agree with the C++.
RUST = ROOT / "rust" / "wuw-board" / "src" / "lib.rs"
RUST_TO_CPP = {
    "CAM_SIOD": "SIOD_GPIO_NUM", "CAM_SIOC": "SIOC_GPIO_NUM",
    "CAM_VSYNC": "VSYNC_GPIO_NUM", "CAM_HREF": "HREF_GPIO_NUM",
    "CAM_D0": "Y2_GPIO_NUM", "CAM_D1": "Y3_GPIO_NUM", "CAM_D2": "Y4_GPIO_NUM",
    "CAM_D3": "Y5_GPIO_NUM", "CAM_D4": "Y6_GPIO_NUM", "CAM_D5": "Y7_GPIO_NUM",
    "CAM_D6": "Y8_GPIO_NUM", "CAM_D7": "Y9_GPIO_NUM",
    "CAM_PCLK": "PCLK_GPIO_NUM", "CAM_XCLK": "XCLK_GPIO_NUM",
    "SD_CMD": "SD_CMD_PIN", "SD_CLK": "SD_CLK_PIN", "SD_D0": "SD_D0_PIN",
    "BTN": "BTN_PIN",
    "LCD_MISO": "LCD_MISO", "LCD_MOSI": "LCD_MOSI", "LCD_SCLK": "LCD_SCLK",
    "LCD_CS": "LCD_CS", "LCD_DC": "LCD_DC", "LCD_RST": "LCD_RST",
    "LCD_BL": "LCD_BL", "TCH_CS": "TCH_CS",
}
if not RUST.exists():
    fails.append("rust/wuw-board/src/lib.rs is missing")
else:
    rust = {m.group(1): int(m.group(2)) for m in re.finditer(
        r"^pub const (\w+): u8 = (\d+);", RUST.read_text(), re.M)}
    cpp = {n: p for d in groups.values() for n, p in d.items()}
    for rname, cname in RUST_TO_CPP.items():
        if rust.get(rname) != cpp.get(cname):
            fails.append("rust/wuw-board %s = %s but C++ %s = %s"
                         % (rname, rust.get(rname), cname, cpp.get(cname)))
    extra = set(rust) - set(RUST_TO_CPP)
    for rname in sorted(extra):
        fails.append("rust/wuw-board %s has no C++ counterpart in pincheck" % rname)
    print("\nrust/wuw-board: %d pins compared" % len(RUST_TO_CPP))

if fails:
    print("\nFAIL")
    for f in fails: print("  " + f)
    sys.exit(1)
print("\nOK - no collisions, nothing on a reserved pin")
