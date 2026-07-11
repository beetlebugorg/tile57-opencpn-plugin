---
id: architecture
title: Architecture
sidebar_position: 4
---

# Architecture

The plugin has three jobs: register a chart class with OpenCPN, turn ENC cells into
tile57's PMTiles so OpenCPN can select them, and draw the chart when OpenCPN asks.
This page covers the first two — the GPU draw is in [Rendering](./rendering.md).

## Plugin entry

`src/tile57_pi.cpp` is the OpenCPN plugin (`Tile57Plugin : opencpn_plugin_118`,
API 1.18). `Init()` advertises its capabilities to OpenCPN:

```
INSTALLS_PLUGIN_CHART | INSTALLS_PLUGIN_CHART_GL |
WANTS_MOUSE_EVENTS | WANTS_CURSOR_LATLON |
WANTS_PREFERENCES | WANTS_OPENGL_OVERLAY_CALLBACK
```

It reports itself as **"tile57 Vector Chart (EXPERIMENTAL)"** (v0.1), warms up
tile57's process-global read-only registries on the main thread (before any bake
thread can race them), and starts a low-frequency timer that nudges OpenCPN to
repaint while charts are baking.

Beyond the chart itself the plugin provides:

- an **OpenGL overlay** that paints the "still baking" veil and a pre-bake progress
  bar (fixed-function GL, so it works before any chart's shaders are loaded);
- a **Build Charts** preferences dialog (bulk cell → PMTiles baking);
- an **object-query** panel on double-click.

## Chart classes

The chart lives in `src/tile57_chart.{h,cpp}`. All the behavior is in one base
class, `ChartTile57`, with two thin concrete subclasses that differ only by file
mask:

| Class | File mask | Source |
| --- | --- | --- |
| `ChartTile57Pmtiles` | `*.pmtiles` | a baked tile57 bundle |
| `ChartTile57Cell` | `*.t57` | a symlink to a live `.000` cell |

OpenCPN's `GetFileSearchMask()` returns a single wildcard (it does **not** split on
`:` or `;`), so each extension needs its own dynamic chart class — hence two
subclasses over one shared base. Only `ChartTile57Pmtiles` is registered with
OpenCPN by default (`GetDynamicChartClassNameArray`); the `.t57` live-cell class
exists but isn't auto-registered, so nothing triggers a slow per-view bake behind
your back. Bulk baking goes through the Build Charts dialog instead.

### Why `PlugInChartBaseExtended`

`ChartTile57` derives from **`PlugInChartBaseExtended`**, not the legacy
`PlugInChartBaseGL`. Extended is what gives quilted vector charts their two-pass GL
render:

1. **`RenderRegionViewOnGLNoText`** — per-patch geometry, clipped to the patch via a
   stencil/scissor mask OpenCPN pre-writes;
2. **`RenderRegionViewOnGLTextOnly`** — one decluttered text pass across the *whole*
   quilt (so labels don't clash at patch seams).

A legacy `BaseGL` chart is wedged through a per-rect compatibility path that shows
seams and doubled text. The plugin maps these two calls onto its renderer's `kBase`
and `kText` passes (see [Rendering](./rendering.md)).

### Scale and coverage

`IsReadyToRender()` **always returns `true`.** Returning "not ready" makes OpenCPN
destroy and re-create the chart every frame — which, with a background bake, re-bakes
forever and never settles. So the chart stays ready and simply draws nothing until
its first tile band arrives.

Coverage comes from the real **M_COVR** polygons (`tile57_chart_coverage`) when the
source has them (live cells), or the chart's bounding box for a baked bundle. Scale
selection (`GetNormalScaleMin/Max`) allows a modest overzoom past native detail and
hands off to a coarser cell when zoomed well out.

## The bake flow

tile57 works on PMTiles, so a raw `.000` cell has to be baked first. The plugin never
blocks OpenCPN's render thread on a bake; baking happens on background threads and the
result is handed over atomically.

```
 .000 cell / .t57 symlink            OpenCPN render thread
        │                                    │
   BakeManager (worker thread)               │
   tile57_bake_chart_bytes                   │
        │  in-memory PMTiles bytes           │
   tile57_chart_open                         │
        │                                    │
   publish(): pending_chart_.exchange(ptr) ──►  adopt_pending():
        │        + CallAfter(Refresh)             pending_chart_.exchange(null)
        ▼                                          -> renderer_.set_chart()
   (never touches renderer_.chart_)
```

- **`BakeManager`** (`src/bake_manager.{h,cpp}`) is a process-wide singleton: the one
  serialized place slow bakes run. It dedups queued work, yields the background queue
  to an interactive `bake_now`, and writes each result to an on-disk cache
  (`~/.cache/tile57/…`) atomically (temp file + `rename`), so a second run of the same
  cell is an instant cache hit. The cache key folds in the cell path, the freshest
  mtime across the `.000` + its `.001…` updates, and a bake-format version.
- **`publish` / `adopt_pending`** is a lock-free handoff: the bake thread stores the
  new chart handle in an atomic pointer and asks OpenCPN to repaint; the render thread
  — the *sole* owner of the live chart — swaps it in at the top of the next frame.
  Each handle is closed exactly once.
- **`build_charts.cpp`** is the separate bulk pipeline behind the Build Charts dialog:
  it walks an ENC root, bakes every cell across a small worker pool into the fixed
  cache directory, and — the only place it touches the OpenCPN API — calls
  `AddChartDirectory` + `ForceChartDBUpdate` when done. Its settings persist to a
  plugin-owned `tile57_pi.ini` (not OpenCPN's config object, which avoids a macOS
  dynamic-lookup crash).

## Object query

Double-clicking runs `tile57_chart_query` against every loaded tile57 chart covering
the cursor and renders the returned S-57 attributes as an HTML panel — the plugin's
equivalent of OpenCPN's native ENC object query.
