---
id: building
title: Building
sidebar_position: 2
---

# Building

The plugin is a small CMake project that links two things you build or vendor
first: the **tile57** static library (the chart engine) and the **OpenCPN plugin
API** (vendored as a git submodule).

## Requirements

- A **C++17** compiler and **CMake ≥ 3.16**
- **wxWidgets** with the `core`, `base`, and `gl` components (the same major
  version OpenCPN uses — 3.2.x)
- **OpenGL** and **GLEW**
- **[tile57](https://github.com/beetlebugorg/tile57)** built as a static library
  (`libtile57.a` + `include/tile57.h`), which needs **Zig 0.16** to build

The OpenCPN plugin API is vendored as the `opencpn-libs` submodule (its
header-only `api-18` target, matching the `opencpn_plugin_118` base class), so **no
separate OpenCPN source checkout is needed**.

## 1. Clone with submodules

```bash
git clone --recursive https://github.com/beetlebugorg/tile57-opencpn-plugin.git
# …or, in an already-cloned checkout:
git submodule update --init
```

## 2. Build tile57 (the chart engine)

The plugin links `libtile57.a`. In a [tile57](https://github.com/beetlebugorg/tile57)
checkout (clone it recursively — it has nested submodules for the IHO S-101
catalogues), run:

```bash
git clone --recursive https://github.com/beetlebugorg/tile57.git
cd tile57
zig build -Doptimize=ReleaseFast   # -> zig-out/lib/libtile57.a + include/tile57.h
```

By default the plugin looks for tile57 at `../tile57` (a sibling directory).
Point elsewhere with `-DTILE57_DIR=/path/to/tile57` when configuring CMake.

## 3. Configure and build the plugin

```bash
cmake -S . -B build -DTILE57_DIR=/path/to/tile57
cmake --build build -j
```

The result is `build/libtile57_pi.so` (Linux) or `build/libtile57_pi.dylib`
(macOS).

### Linux

The dependencies are all in the distro package manager, e.g. on Debian/Ubuntu:

```bash
sudo apt-get install -y \
  build-essential cmake \
  libwxgtk3.2-dev libwxgtk-gl3.2-dev \
  libglew-dev libgl1-mesa-dev
```

A plugin module on Linux is allowed to leave the OpenCPN base-class and
`PI_*` helper symbols undefined; they resolve when OpenCPN `dlopen`s the plugin.

### macOS

Point CMake at Homebrew so it finds GLEW and wxWidgets:

```bash
cmake -S . -B build \
  -DTILE57_DIR=/path/to/tile57 \
  -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build build -j
```

A macOS plugin must use the **same wxWidgets** as `OpenCPN.app`, or a second
wxWidgets loads and OpenCPN silently drops the plugin. The build compiles against
your (Homebrew) wx, then a post-build step (`cmake/fix-macos-libs.sh`) repoints the
plugin's wx references to the app's bundled copy **and re-signs the binary ad-hoc**
(the `install_name_tool` retarget invalidates the code signature; an unsigned
plugin is `SIGKILL`ed at load). It looks for the app at `OCPN_APP`, default
`/Applications/OpenCPN.app`:

```bash
cmake -S . -B build -DOCPN_APP=/path/to/OpenCPN.app ...
```

## Continuous integration

`.github/workflows/ci.yml` builds the plugin for **Linux** and **Windows** on every
push. It builds tile57 with Zig first, then configures and builds the plugin, and
uploads the resulting module as a workflow artifact.

## Next

Once you have a built module, see [Getting Started](./getting-started.md) to install
it into OpenCPN and load a chart.
