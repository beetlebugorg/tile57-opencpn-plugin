---
id: rendering
title: Rendering
sidebar_position: 5
---

# Rendering

The renderer draws tile57's draw-ready GPU scene inside OpenCPN's render loop. tile57
portrays a whole view once into vertex, index and quad buffers that are already
tessellated, already in S-52 paint order, and already split into ranges that each
draw with one pipeline. The plugin uploads those buffers and draws them under a
per-frame transform. A pan, zoom or rotation is a uniform change; the scene is
rebuilt only when the view leaves its coverage or the mariner settings change.

The code is split by responsibility:

| File | Role |
| --- | --- |
| `src/chart_renderer.*` | the host-facing API, the rebuild policy, and the render targets |
| `src/scene_build.*` | the CPU build (`tile57_chart_gpu_scene`) and its worker thread |
| `src/gpu_scene.*` | a resident scene: GL buffers, pattern textures, batched draw lists |
| `src/chart_programs.*` | the GLSL 1.20 programs and vertex stream bindings |
| `src/chart_atlases.*` | the symbol and glyph atlas textures and the halo colors |
| `src/gl_objects.h` | move-only owners for GL objects |

## Passes

OpenCPN calls a quilted vector chart twice per frame, and the renderer maps each call
onto a `Pass`:

- `RenderRegionViewOnGLNoText` is `kBase`: everything but text, clipped to the chart's
  quilt patch.
- `RenderRegionViewOnGLTextOnly` is `kText`: text only, drawn above OpenCPN's overlays.

A single unquilted chart uses `kAll`. One scene serves every pass; the pass selects
which ranges are batched.

## Scenes and coverage

A scene is portrayed for a view 1.25 times the viewport on each axis, at the view's
zoom. It stays valid while the live view keeps the whole viewport inside that box and
the zoom stays within 0.3 of the build zoom. Vertices are uploaded relative to the
scene's build center so single-precision floats hold their accuracy at harbor zoom.

The first scene for a chart is built on the render thread, because nothing is on
screen yet. Every later build runs on a worker thread while the current scene keeps
drawing. When a build finishes, the worker asks the canvas for one redraw, and the
next render pass adopts the new scene. One request is pending at a time; a newer
request replaces one that has not started.

The rebuild policy:

- a settings change or a pan out of the coverage box rebuilds at once;
- a zoom drift past the band waits while a gesture is in progress, up to one full zoom
  level, then rebuilds when the view settles;
- a zoom-out is built 0.15 levels further out than the view, so its coverage is a
  superset of every view on the way there.

## Draw lists

`tile57_gpu_batch` turns a scene's ranges into draw calls, merging neighbors that share
a pipeline, atlas and pattern. The plugin caches one draw list per combination of pass,
text and soundings switches, available atlases, and depth mode.

When a depth buffer is available, opaque fills are drawn first, front to back, with
depth writes on, and the depth test drops the fragments later paint would cover. The
remaining ranges then draw in paint order with the depth test on and writes off. The
supersample target always has a depth buffer; direct rendering uses the host's when it
has one.

## The vertex model

Every vertex is transformed in the shader as:

```
screen = R * (aWorld * uScale) + uOrigin + post
```

- `aWorld` is the vertex's world position relative to the scene's build center;
  `uScale` is framebuffer px per world unit; `uOrigin` places the center on screen;
  `R` is the view rotation.
- `post` is a screen-space offset in device px: zero for area fills, the half-width
  for line edges, the corner for a glyph or symbol quad. A map-aligned offset is
  rotated with the chart; a viewport-aligned one stays upright. A text run flagged to
  flip turns 180 degrees when its tangent would read into the left half of the screen.
- `aScamin` and `aDispCat` gate visibility on the GPU against the live display scale
  and category switches, so neither forces a rebuild.
- `aDepth` is tile57's paint-order key.

Colors are per vertex. SDF text carries a halo width per vertex and draws the halo in
the scheme's background color.

## Atlases

The symbol atlas is baked per pixel ratio and color scheme; the SDF glyph atlases are
baked per face (regular, bold, italic). Each is uploaded on first use and shared by
every chart in the process.

## Antialiasing

When the view is settled, the scene is rendered into a supersampled framebuffer (2x by
default) and composited down. During a pan or zoom the renderer draws directly into
OpenCPN's buffer, because an offscreen composite tears against OpenCPN's accelerated
pan. Supersampling is off on software renderers.

## OpenCPN's GL state

OpenCPN mixes fixed-function drawing with the plugin's programs. At the end of every
draw the renderer disables its vertex attribute arrays, unbinds buffers and textures,
clears the active program, and restores the depth mask.

## Environment variables

| Variable | Effect |
| --- | --- |
| `TILE57_SS=<n>` | supersample factor (default 2; 1 disables AA) |
| `TILE57_NOSS` | draw directly, no offscreen target |
| `TILE57_SIZE` / `TILE57_CALIB` | physical symbol and text size calibration |
| `TILE57_DECLUTTER=<levels>` | SCAMIN cull bias |
| `TILE57_DEBUG` | log scene builds and the render entry state |
