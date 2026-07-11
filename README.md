# tile57 OpenCPN plugin

An OpenCPN plugin that draws S-57/S-101 electronic navigational charts using
[tile57](https://github.com/beetlebugorg/tile57)'s live S-52 portrayal, rendered as
vector geometry on the GPU.

📖 **[Documentation](https://beetlebugorg.github.io/tile57-opencpn-plugin/)** —
building, getting started, architecture, and rendering internals.

> [!WARNING]
> **Experimental — not for navigation.** This plugin and the tile57 engine behind it
> are coded almost entirely with AI (Claude) and human-reviewed — an experiment in
> implementing a large, complex specification, not a certified or tested navigation
> product. The plugin marks its output accordingly. Do not rely on it for real-world
> navigation.

![tile57 rendering Annapolis harbor](docs/static/img/annapolis-harbor.png)

## How it works

The plugin installs a **first-class GL vector chart** (`ChartTile57`, a
`PlugInChartBaseExtended`) — not an overlay. OpenCPN discovers a tile57 chart by file
mask, adds it to the chart database, and drives it like any native chart: chart bar,
quilting, and scale transitions included. OpenCPN owns the draw order.

When OpenCPN renders the chart, the plugin follows the MapLibre "bake once, compose on
demand" model:

1. it converts OpenCPN's `PlugIn_ViewPort` into a tile57 camera (centre + a continuous
   web-mercator zoom);
2. for each visible tile, it runs tile57's S-52 portrayal once
   (`tile57_chart_tile_surface`), tessellates the resolved primitives to GPU buffers,
   and **caches** them keyed by `(z, x, y)` (LRU, budgeted so a cold view fills in
   progressively rather than freezing);
3. it composes the view from the cached tiles as a GPU transform — so panning and
   zooming reuse geometry instead of re-portraying;
4. labels are portrayed once for the whole view with a single declutter grid (and
   cached the same way) so text doesn't clash at tile seams.

ENC cells are baked to `*.pmtiles` bundles up front (via the plugin's Build Charts
dialog or the `tile57` CLI); OpenCPN then loads those bundles like any chart directory.
See the [architecture docs](https://beetlebugorg.github.io/tile57-opencpn-plugin/architecture)
for the full picture.

## Layout

```
src/tile57_pi.cpp        OpenCPN plugin entry (create_pi / plugin class); registers the chart + dialogs
src/tile57_chart.*       ChartTile57Pmtiles (PlugInChartBaseExtended) — opens a baked *.pmtiles bundle
src/chart_renderer.*     tiled portray -> tessellate -> cache -> compose on the GPU
src/build_charts.*       Build Charts dialog: bulk-bake an ENC root to *.pmtiles
src/gl.h                 GL headers (GLEW) + GLSL version prologue
third_party/earcut.hpp   polygon tessellation (Mapbox earcut, ISC)
opencpn-libs/            OpenCPN plugin API (git submodule; api-18 -> ocpn::api)
```

## Quick build

Requires a C++17 compiler, CMake ≥ 3.16, wxWidgets (with `gl`), OpenGL and GLEW, plus
**tile57** built as a static library. Clone with submodules, build tile57, then build
the plugin:

```sh
git clone --recursive https://github.com/beetlebugorg/tile57-opencpn-plugin.git

# build the tile57 engine (needs Zig 0.16; sibling directory by default)
git clone --recursive https://github.com/beetlebugorg/tile57.git
( cd tile57 && zig build -Doptimize=ReleaseFast )

# build the plugin
cmake -S tile57-opencpn-plugin -B build -DTILE57_DIR=$PWD/tile57
cmake --build build -j
```

The result is `build/libtile57_pi.so` (`.dylib` on macOS). See the
[building guide](https://beetlebugorg.github.io/tile57-opencpn-plugin/building) for
Linux/macOS specifics (macOS needs a wxWidgets retarget + re-sign against
`OpenCPN.app`), and [getting started](https://beetlebugorg.github.io/tile57-opencpn-plugin/getting-started)
for installing and loading charts.

## Documentation

The full docs live at
**<https://beetlebugorg.github.io/tile57-opencpn-plugin/>** and are built from
`docs/` (Docusaurus) and deployed by GitHub Actions. Build them locally with:

```sh
cd docs && npm install && npm start
```

## Licensing

The plugin is GPL-2.0-or-later (matching OpenCPN's plugin ecosystem). tile57 is MIT.
Bundled third-party code is listed in `THIRD_PARTY_LICENSES.md`. Note that tile57
embeds the IHO S-101 Portrayal Catalogue (© IHO); confirm its redistribution terms
before distributing binaries.

## Limitations

OpenGL canvas only; web-mercator (no rotation/course-up yet); charts must be baked to
PMTiles (unencrypted cells). Portrayal fidelity follows tile57. See the
[known limitations](https://beetlebugorg.github.io/tile57-opencpn-plugin/limitations).
