---
id: intro
title: Introduction
slug: /
sidebar_position: 1
---

# tile57 OpenCPN plugin

:::warning Not for navigation

This plugin and the [tile57](https://github.com/beetlebugorg/tile57) engine behind
it are coded almost entirely with AI (Claude) and human-reviewed — an experiment in
implementing a large, complex specification (IHO S-52/S-101), **not** a certified or
tested navigation product. The plugin marks its output accordingly (the
"EXPERIMENTAL / NOT FOR NAVIGATION" note rides in the chart name OpenCPN shows). Do
not rely on it for real-world navigation. See [Known limitations](./limitations.md).

:::

An **OpenCPN plugin** that draws S-57/S-101 electronic navigational charts using
[tile57](https://github.com/beetlebugorg/tile57)'s live S-52 portrayal, rendered as
vector geometry on the GPU.

![tile57 S-57/S-101 charts rendered in OpenCPN](/img/hero.png)

The plugin installs a **first-class GL vector chart**, not an overlay. OpenCPN
discovers a tile57 chart by file mask, adds it to the chart database, and drives it
like any native chart — chart bar, quilting, and scale transitions included. When
OpenCPN asks the chart to draw, the plugin runs tile57's S-52 portrayal, tessellates
the resulting vector primitives, caches them as per-tile GPU buffers, and composes
the view — the same "bake once, compose on demand" model a web MapLibre client uses,
but inside OpenCPN's own render loop.

## How it fits together

```
 ENC cells (.000)          OpenCPN                      tile57 engine
      │                       │                              │
  Build Charts               │                              │
  (bake up front)            │                              │
      ▼                      │   RenderRegionViewOnGL        │
 PMTiles bundles ────────────┤─────────────────────────────►│ S-52 portrayal
      │                      │   per-tile portray + labels   │ (draw callbacks)
      ▼                      │◄─────────────────────────────┤
  chart directory            │   per-tile GPU cache          │
      └──────────────────────►   composed on the GPU         │
```

Charts are baked to PMTiles bundles once (via the plugin's Build Charts dialog), then
OpenCPN loads them like any chart directory and drives the GPU render.

- **[tile57](https://github.com/beetlebugorg/tile57)** is the chart engine: it reads
  S-57, adapts to the S-101 data model, runs the official IHO S-101 Portrayal
  Catalogue, and emits resolved draw primitives. This plugin embeds tile57 as a
  static library and calls it through its C ABI.
- **OpenCPN** owns the canvas, the quilt, and the draw order. The plugin is a
  `PlugInChartBaseExtended` chart class; OpenCPN selects and renders it exactly as it
  would a native ENC.

## Where to go next

- [**Building**](./building.md) — dependencies, the tile57 static library, and the
  CMake build (Linux and macOS).
- [**Getting Started**](./getting-started.md) — bake or link a chart, add it to
  OpenCPN, and enable OpenGL.
- [**Architecture**](./architecture.md) — the chart class, the bake flow, and how the
  plugin plugs into OpenCPN's quilted render.
- [**Rendering**](./rendering.md) — the tiled portray/cache/compose pipeline, the
  whole-view label pass, and the GPU vertex model.
- [**Settings**](./settings.md) — how OpenCPN's S-52 display options (safety
  contour, depth shades, light descriptions, …) map onto tile57's mariner settings.
- [**Known limitations**](./limitations.md) — what does not work yet.
