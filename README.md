# tile57 OpenCPN plugin

An OpenCPN plugin that draws S-57/S-101 electronic navigational charts using
[tile57](https://github.com/beetlebugorg/tile57)'s live S-52 portrayal, rendered
as vector geometry on the GPU.

> **EXPERIMENTAL — NOT FOR NAVIGATION.** tile57 is experimental software. This
> plugin marks its output accordingly and must not be used for navigation.

## How it works

The plugin registers as an OpenGL overlay. Each frame it:

1. converts OpenCPN's `PlugIn_ViewPort` into a tile57 camera (centre + a
   continuous web-mercator zoom derived from the view's ground resolution);
2. calls `tile57_chart_render_view_cb`, which runs tile57's portrayal pipeline
   and paints the view through a callback canvas — flattened, resolved
   primitives (fills, strokes, glyph outlines) in screen-pixel space;
3. tessellates those primitives to triangles (fills via ear clipping, strokes
   expanded to quads) and draws them on the GPU;
4. draws a persistent "NOT FOR NAVIGATION" banner over the result.

tile57 portrayal is only re-run when the view or mariner settings change; between
changes the cached geometry is replayed.

## Layout

```
src/tile57_pi.cpp        OpenCPN plugin entry (create_pi / plugin class)
src/chart_renderer.*     callback canvas -> triangles -> GPU
src/safety_overlay.*     the NOT FOR NAVIGATION banner (own GL program + font)
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

## Installing and running

Install into OpenCPN's user plugin directory and enable it in
Options → Plugins:

```sh
mkdir -p ~/.local/lib/opencpn
cp build/libtile57_pi.so ~/.local/lib/opencpn/
```

The chart is a baked tile57 bundle's `chart.pmtiles`. Point the plugin at it with
the `OPENCPN_T57_CHART` environment variable (there is a default path otherwise):

```sh
# bake an unencrypted S-57 cell to a tile57 bundle
tile57 bake CELL.000 -o /path/to/bundle
export OPENCPN_T57_CHART=/path/to/bundle/tiles/chart.pmtiles
```

Navigate OpenCPN to the chart's area at a matching scale; the vector chart is
drawn as an overlay.

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
