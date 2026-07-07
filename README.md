# tile57 OpenCPN plugin

An OpenCPN plugin that draws S-57/S-101 electronic navigational charts using
[tile57](https://github.com/beetlebugorg/tile57)'s live S-52 portrayal, rendered
as vector geometry on the GPU.

> [!WARNING]
> **Experimental — not for navigation.** This plugin and the tile57 engine behind it
> are coded almost entirely with AI (Claude) and human-reviewed — an experiment in
> implementing a large, complex specification, not a certified or tested navigation
> product. The plugin marks its output accordingly. Do not rely on it for real-world
> navigation.

## How it works

The plugin installs a **first-class GL vector chart** (`ChartTile57`, a
`PlugInChartBaseGL`). OpenCPN discovers a baked tile57 bundle's `chart.pmtiles`
by file mask (`*.pmtiles`), adds it to the chart database, and drives it like any
native chart — chart bar, quilting and scale transitions included. The plugin is
not an overlay; OpenCPN owns the draw order.

When OpenCPN renders the chart (`RenderRegionViewOnGL`) it:

1. converts OpenCPN's `PlugIn_ViewPort` into a tile57 camera (centre + a
   continuous web-mercator zoom derived from the view's ground resolution);
2. calls `tile57_chart_render_view_cb`, which runs tile57's portrayal pipeline
   and paints the view through a callback canvas — flattened, resolved
   primitives (fills, strokes, glyph outlines) in screen-pixel space;
3. tessellates those primitives to triangles (fills via ear clipping, strokes
   expanded to quads) and draws them on the GPU.

tile57 portrayal is only re-run when the view or mariner settings change; between
changes the cached geometry is replayed. At `Init` the chart reads the bundle's
metadata (`tile57_chart_get_info`) for its extent, native scale and coverage.

Since tile57 is experimental, the "NOT FOR NAVIGATION" warning rides in the
chart's name and description, which OpenCPN shows in the chart bar and chart
info.

## Layout

```
src/tile57_pi.cpp        OpenCPN plugin entry (create_pi / plugin class); installs the chart
src/tile57_chart.*       ChartTile57 — the PlugInChartBaseGL chart class
src/chart_renderer.*     callback canvas -> triangles -> GPU
src/gl.h                 GL headers + GLSL version prologue
third_party/earcut.hpp   polygon tessellation (Mapbox earcut, ISC)
opencpn-libs/            OpenCPN plugin API (git submodule; api-18 -> ocpn::api)
```

## Building

Requires a C++17 compiler, CMake ≥ 3.16, wxWidgets (with the `gl` component),
OpenGL, GLEW and GLU, plus **tile57** built as a static library — in a tile57
checkout, run `zig build` to produce `zig-out/lib/libtile57.a` and
`include/tile57.h`.

The OpenCPN plugin API is vendored as the `opencpn-libs` submodule (its
header-only `api-18` target, matching the `opencpn_plugin_118` base class), so no
separate OpenCPN source checkout is needed. Clone with submodules — or, in an
existing checkout, initialise them:

```sh
git clone --recursive <repo-url>
git submodule update --init   # in an already-cloned checkout
```

By default the build looks for tile57 at `../tile57`; override with a cache
variable as needed. On macOS, point CMake at Homebrew so it finds GLEW/wxWidgets:

```sh
cmake -S . -B build \
  -DTILE57_DIR=/path/to/tile57 \
  -DCMAKE_PREFIX_PATH="$(brew --prefix)"   # macOS/Homebrew only
cmake --build build -j
```

The result is `build/libtile57_pi.so` (`libtile57_pi.dylib` on macOS).

On **macOS**, a plugin must use the *same* wxWidgets as OpenCPN.app, or a second
wxWidgets loads and OpenCPN silently drops the plugin. The build compiles against
your (Homebrew) wx, then a post-build step (`cmake/fix-macos-libs.sh`) repoints
the plugin's wx references to the app's bundled copy. It looks for the app at
`OCPN_APP`, default `/Applications/OpenCPN.app`; override if it lives elsewhere:

```sh
cmake -S . -B build -DOCPN_APP=/path/to/OpenCPN.app ...
```

## Installing and running

Install into OpenCPN's user plugin directory and enable it in
Options → Plugins:

```sh
# Linux
mkdir -p ~/.local/lib/opencpn
cp build/libtile57_pi.so ~/.local/lib/opencpn/

# macOS
mkdir -p ~/Library/Application\ Support/OpenCPN/Contents/PlugIns
cp build/libtile57_pi.dylib ~/Library/Application\ Support/OpenCPN/Contents/PlugIns/
```

The chart is a baked tile57 bundle's `chart.pmtiles`. Because the plugin installs
a real chart, you point OpenCPN at it the same way you would any chart — add the
folder holding `chart.pmtiles` as a chart directory:

```sh
# bake an unencrypted S-57 cell to a tile57 bundle
tile57 bake CELL.000 -o /path/to/bundle
# -> /path/to/bundle/tiles/chart.pmtiles
```

In OpenCPN: **Options → Charts → Chart Files**, add `/path/to/bundle/tiles`
(the folder containing `chart.pmtiles`), and **Scan Charts / Prepare**. The
tile57 chart then appears in the chart bar. Navigate to its area and OpenCPN
selects and draws it like a native chart.

OpenCPN must be running with **OpenGL enabled** (Options → Display → Advanced) —
the chart renders through the GPU; on the non-GL canvas it draws nothing.

## Licensing

The plugin is GPL-2.0-or-later (matching OpenCPN's plugin ecosystem). tile57 is
MIT. Bundled third-party code is listed in `THIRD_PARTY_LICENSES.md`. Note that
tile57 embeds the IHO S-101 Portrayal Catalogue (© IHO); confirm its
redistribution terms before distributing binaries.

## Limitations

- Text labels are filled from glyph outlines without an even-odd rule, so letter
  counters are not yet cut out.
- Chart is loaded from a baked tile57 bundle (`.pmtiles`); direct live rendering
  of a raw `.000` cell is not wired up here.
- Web Mercator only; no rotation/course-up handling yet.
