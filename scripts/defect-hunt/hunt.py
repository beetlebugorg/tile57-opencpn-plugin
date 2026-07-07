#!/usr/bin/env python3
"""Render each East Coast cell at several zooms and flag rendering defects.

Detects:
  * MAGENTA  — the FALLBACK colour (255,0,255): an unresolved S-52 colour token.
  * RENDER_FAIL — the renderer errored / produced no image (broken cell/tile).
  * BLANK    — a view that is ~entirely one colour (possible missing tile / no data).

Uses tile57's pixel path (`tile57 png --view`), which shares the data + portrayal
with the plugin, so data/colour defects surface here. Prints one line per defect.

Usage: hunt.py <pmtiles>...        (TILE57_BIN overrides the baker path)
"""
import subprocess, sys, os
from concurrent.futures import ThreadPoolExecutor
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
T57 = os.environ.get("TILE57_BIN", os.path.expanduser("~/Projects/tile57-c-callbacks/zig-out/bin/tile57"))
INFO = os.path.join(HERE, "chart_info")
SIZE = "500x500"


def defects_for(rec, idx):
    path, minz, maxz, w, s, e, n = rec.split(",")
    minz, maxz = int(minz), int(maxz)
    w, s, e, n = float(w), float(s), float(e), float(n)
    name = os.path.basename(path)[:-8]
    out = []
    # Sample a few interior points at the cell's native (max) + mid zoom, where the
    # view sits INSIDE the cell — so transparent = missing tile / no data (water is
    # an opaque fill, so it is not flagged), and min_zoom's outside-cell case is
    # avoided. Points are inset from the bbox to dodge irregular-coverage edges.
    pts = [(0.5, 0.5), (0.3, 0.3), (0.7, 0.7), (0.3, 0.7), (0.7, 0.3)]
    for z in sorted({maxz, (minz + maxz + 1) // 2}):
        for j, (fx, fy) in enumerate(pts):
            lon = w + (e - w) * fx
            lat = s + (n - s) * fy
            tmp = f"/tmp/hunt_{idx}_{z}_{j}.png"
            r = subprocess.run([T57, "png", path, "--view", f"{lon},{lat},{z}",
                                "--size", SIZE, "-o", tmp], capture_output=True)
            if r.returncode != 0 or not os.path.exists(tmp):
                out.append(f"{name} z{z} RENDER_FAIL")
                continue
            im = Image.open(tmp).convert("RGBA")
            cols = im.getcolors(im.width * im.height) or []
            total = im.width * im.height
            mag = sum(c for c, px in cols if px[0] > 240 and px[1] < 30 and px[2] > 240)
            clear = sum(c for c, px in cols if px[3] < 10)
            os.remove(tmp)
            if mag > 15:
                out.append(f"{name} z{z}@({fx},{fy}) MAGENTA {mag}px")
            if clear / total > 0.6:
                out.append(f"{name} z{z}@({fx},{fy}) NODATA {clear/total:.0%} transparent")
    return out


def main(cells):
    info = subprocess.run([INFO] + cells, capture_output=True, text=True)
    recs = [l for l in info.stdout.strip().splitlines() if l]
    defects = []
    with ThreadPoolExecutor(max_workers=8) as ex:
        for res in ex.map(lambda t: defects_for(t[1], t[0]), enumerate(recs)):
            defects.extend(res)
    for d in defects:
        print(d)
    print(f"--- {len(defects)} defects across {len(recs)} cells "
          f"({len(cells) - len(recs)} without bounds)")


if __name__ == "__main__":
    main(sys.argv[1:])
