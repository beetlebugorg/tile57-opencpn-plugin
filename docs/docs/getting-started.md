---
id: getting-started
title: Getting Started
sidebar_position: 3
---

# Getting Started

## 1. Install the plugin

Copy the built module into OpenCPN's user plugin directory and enable it in
**Options → Plugins**:

```bash
# Linux
mkdir -p ~/.local/lib/opencpn
cp build/libtile57_pi.so ~/.local/lib/opencpn/

# macOS
mkdir -p ~/Library/Application\ Support/OpenCPN/Contents/PlugIns
cp build/libtile57_pi.dylib ~/Library/Application\ Support/OpenCPN/Contents/PlugIns/
```

The plugin appears as **"tile57 Vector Chart (EXPERIMENTAL)"**.

## 2. Enable OpenGL

The chart renders through the GPU. In **Options → Display → Advanced**, make sure
**OpenGL** is enabled — on the non-GL canvas the chart draws nothing.

## 3. Add charts

The plugin installs a real chart class, so you point OpenCPN at charts the same way
you would any native chart. There are two ways to produce them.

### Option A — the Build Charts dialog (recommended)

Open the plugin's preferences (**Options → Plugins → tile57 → Preferences**) to get
the **Build Charts** dialog. Point it at a folder of S-57 cells (an ENC root full of
`.000` files). The plugin bakes each cell — on background threads — to a PMTiles
bundle in its cache directory (`~/.cache/tile57/charts`), then adds that directory to
OpenCPN and refreshes the chart database for you. When baking finishes the charts
appear in the chart bar.

### Option B — bake with the tile57 CLI

Bake a cell yourself with the [tile57](https://github.com/beetlebugorg/tile57) CLI,
then add the output folder as a chart directory:

```bash
# bake an unencrypted S-57 cell (or a whole ENC root) to a PMTiles bundle
tile57 bake CELL.000 -o /path/to/bundle
# -> /path/to/bundle/tiles/<CELL>.pmtiles
```

In OpenCPN: **Options → Charts → Chart Files**, add the folder containing the
`.pmtiles` files, and **Scan Charts / Prepare**. (If OpenCPN doesn't pick up the new
charts, it may prompt to update the chart database — accept it.)

The tile57 chart then appears in the chart bar. Navigate to its area and OpenCPN
selects and draws it like a native chart, including quilting with adjacent cells.

## 4. Query a feature

**Double-click** a feature to open an object-query panel — the S-57 attributes for
whatever the plugin's charts cover under the cursor, the same information a native
ENC "Object Query" gives you.

## Baking feedback

Baking happens up front, in the **Build Charts** dialog — it shows a progress bar as it
works through the cells, then registers the chart directory when it finishes. Once
charts are baked, OpenCPN opens the `.pmtiles` bundles directly and they render
immediately; there is no per-view baking while you navigate.

## Next

- [Architecture](./architecture.md) — how the plugin plugs into OpenCPN and bakes
  charts.
- [Settings](./settings.md) — the S-52 display options the plugin honors.
