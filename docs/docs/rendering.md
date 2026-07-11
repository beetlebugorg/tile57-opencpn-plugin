---
id: rendering
title: Rendering
sidebar_position: 5
---

# Rendering

The renderer (`src/chart_renderer.cpp`) turns tile57's S-52 portrayal into GPU
geometry and draws it inside OpenCPN's render loop. The model mirrors a web MapLibre
client: **portray and tessellate each tile once, cache it, and compose the view from
cached tiles** — so panning and zooming are GPU transforms, not re-portrayals.

## Two passes

OpenCPN calls the chart twice per frame for a quilted vector chart, and the renderer
maps each onto a `Pass`:

- `RenderRegionViewOnGLNoText` → **`kBase`** — tiled geometry, clipped to the chart's
  quilt patch.
- `RenderRegionViewOnGLTextOnly` → **`kText`** — the whole-view label pass, drawn once
  across the quilt.

(A single, unquilted chart uses `kAll`, which does both.)

## Tiled geometry

`render_tiled()` enumerates the tiles visible in the current view (at the baked zoom
band nearest the view's zoom, clamped to the chart's coverage), and for each:

- **cache hit** — draws the tile's cached GPU buffers;
- **cache miss** — calls `ensure_tile()`, which portrays that one tile through
  tile57's `tile57_chart_tile_surface`, tessellates the resolved primitives, and
  stores per-layer vertex buffers keyed by `(z, x, y)`.

A tile holds up to seven layer buffers — area fills, lines, tessellated symbols,
textured sprites, and area-fill patterns (text and glyph layers are handled by the
label pass, below). Vertices are stored **relative to the tile's north-west world
corner**, keeping the floating-point coordinates small and the buffers reusable across
any view.

Drawing batches by shader program — one program bind per layer, then an inner loop
over the visible tiles, each placed with its own origin uniform — so a full view is a
handful of state changes regardless of tile count.

### Budget and eviction

A cold view (say, a big zoom-out) can bring many tiles into view at once. Portraying
them all in one frame would hitch, so `render_tiled` **portrays at most a few per
frame** (2 while the view is moving, 8 when settled) and flags the rest pending; the
chart then asks OpenCPN for another redraw, so the view *fills in progressively*
instead of freezing. Cached tiles are always drawn.

The cache is bounded by an **LRU** (cap 512 tiles): tiles drawn this frame carry the
newest timestamp and survive; the least-recently-drawn are evicted and their buffers
freed. The whole cache is invalidated when the mariner settings change (they change
the portrayal) or the chart is swapped.

## The label pass

Labels are **not** tiled. tile57's declutter grid is per-surface, so decluttering each
tile in isolation drops labels at tile seams — most visibly, light descriptions. So
text is portrayed **once for the whole view** with a single shared declutter grid
(`portray_view_labels` → `tile57_chart_surface`, text only), into one pair of
vertex/glyph buffers referenced to the view centre.

That whole-view portray is the most expensive per-frame operation (it decodes the
PMTiles and declutters every sounding), so it is **cached like the tiles**: it
re-portrays only when the view has settled at a new place or zoom, and during a pan or
zoom gesture it reuses the cached label buffers, drawn with a transformed origin so
they ride the view. A refresh deferred mid-gesture is picked up once motion stops (the
same "ask for one more redraw" mechanism the tiles use).

Labels are drawn at the **true** zoom, not the SCAMIN-biased cull zoom used for
symbols — text and soundings already self-declutter, so biasing them would hide labels
(lights first) on high-DPI displays.

## The vertex model

Every vertex is transformed in the shader as:

```
screen = aWorld * uScale + uOrigin + aPost
```

- **`aWorld`** — the vertex's world offset from its tile/view reference (small f32);
  `uScale` is pixels per world unit; `uOrigin` places the reference on screen.
- **`aPost`** — a post-transform, screen-pixel offset: zero for area fills, the
  perpendicular half-width for line quads, and the local glyph/symbol quad corner for
  anchored text and symbols — so symbols and text keep a constant on-screen size while
  the map scales.
- **`aThresh`** — a per-vertex **SCAMIN** cull threshold. The shader discards the
  vertex when `uZoom < aThresh`, so features drop out at the scale S-52 says they
  should, entirely on the GPU.

Five shader programs (compiled once, process-wide, and shared by every chart
instance) cover the layers: solid geometry, textured sprites, tiled-pattern area
fills, SDF text, and a fullscreen blit for compositing.

## Antialiasing

When the view is **settled**, the scene is rendered into an offscreen
super-sampled framebuffer (2× by default) and composited back down — antialiasing
tessellated edges and text. During a pan or zoom the renderer draws **directly** into
OpenCPN's buffer instead (offscreen compositing tore against OpenCPN's accelerated-pan
strip updates); the crisp AA pops back in when motion stops. Super-sampling is
disabled automatically on software renderers (llvmpipe and friends).

## Playing nice with OpenCPN's GL

OpenCPN mixes its own fixed-function and `gluTess` drawing with the plugin's modern
GL. At the end of every frame the renderer **hard-resets GL state** — disables its
vertex-attribute arrays, unbinds buffers and textures, and clears the active program —
so OpenCPN's immediate-mode clip-region drawing can't read the plugin's vertex buffer
out of bounds (a crash seen on macOS).

## Environment knobs

A few environment variables tune the renderer for debugging and calibration:

| Variable | Effect |
| --- | --- |
| `TILE57_SS=<n>` | super-sample factor (default 2; 1 disables AA) |
| `TILE57_NOSS` | force direct rendering, no offscreen FBO |
| `TILE57_SIZE` / `TILE57_CALIB` | physical symbol/text size calibration |
| `TILE57_DECLUTTER=<levels>` | override the SCAMIN cull bias |
| `TILE57_DEBUG` | log the per-zoom projection / cull state |
