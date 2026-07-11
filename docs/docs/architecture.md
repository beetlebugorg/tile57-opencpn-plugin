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
WANTS_MOUSE_EVENTS | WANTS_CURSOR_LATLON | WANTS_PREFERENCES
```

It reports itself as **"tile57 Vector Chart (EXPERIMENTAL)"** (v0.1) and warms up
tile57's process-global read-only registries on the main thread before the chart-DB
scan first creates a chart.

Beyond the chart itself the plugin provides:

- a **Build Charts** preferences dialog (bulk cell → PMTiles baking);
- an **object-query** panel on double-click.

## Chart classes

The chart lives in `src/tile57_chart.{h,cpp}`. All the behavior is in the base class
`ChartTile57`; a thin concrete subclass sets the file mask OpenCPN scans for:

| Class | File mask | Source |
| --- | --- | --- |
| `ChartTile57Pmtiles` | `*.pmtiles` | a baked tile57 bundle |

OpenCPN's `GetFileSearchMask()` returns a single wildcard (it does **not** split on
`:` or `;`), so the mask lives on a dedicated dynamic chart class, registered by name
through `GetDynamicChartClassNameArray`. Every chart is a pre-baked bundle; you produce
them with the Build Charts dialog (below).

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

`IsReadyToRender()` **always returns `true`.** A baked bundle opens synchronously in
`Init()` — its metadata and coverage load immediately — so the chart is ready at once.
(Reporting "not ready" would make OpenCPN destroy and re-create the chart every frame.)

Coverage comes from the real **M_COVR** polygons (`tile57_chart_coverage`) when the
bundle embeds them, else the chart's bounding box. Scale selection
(`GetNormalScaleMin/Max`) allows a modest overzoom past native detail and hands off to
a coarser cell when zoomed well out.

## Baking charts

tile57 renders from PMTiles, so ENC cells are baked to bundles **up front** — there is
no per-view baking. The **Build Charts** dialog (`src/build_charts.cpp`) is the
pipeline:

- point it at an ENC root; it walks the tree and bakes every `.000` cell across a small
  worker pool, calling tile57's `tile57_bake_chart_bytes` directly (each bundle embeds
  the cell's real M_COVR coverage);
- output goes to a fixed cache directory (`~/.cache/tile57/charts`), written atomically
  (temp file + `rename`);
- when the sweep finishes it calls `AddChartDirectory` + `ForceChartDBUpdate` — the only
  place the baker touches the OpenCPN API — so the charts appear without a manual
  rescan. Its settings persist to a plugin-owned `tile57_pi.ini` (not OpenCPN's config
  object, which avoids a macOS dynamic-lookup crash).

Once baked, `ChartTile57Pmtiles::Init` simply opens the `.pmtiles` archive directly and
the chart renders immediately.

## Object query

Double-clicking runs `tile57_chart_query` against every loaded tile57 chart covering
the cursor and renders the returned S-57 attributes as an HTML panel — the plugin's
equivalent of OpenCPN's native ENC object query.
