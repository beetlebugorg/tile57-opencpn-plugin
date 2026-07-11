---
id: limitations
title: Known Limitations
sidebar_position: 7
---

# Known Limitations

:::warning Not for navigation

This plugin and the tile57 engine are an AI-written, human-reviewed experiment, not a
certified navigation product. Do not rely on them for real-world navigation.

:::

## Plugin

- **OpenGL only.** The chart renders through the GPU. On OpenCPN's non-GL (software)
  canvas it draws nothing — enable OpenGL in **Options → Display → Advanced**.
- **Web Mercator only.** There is no chart rotation or course-up handling yet; the
  view is north-up web-mercator.
- **Charts must be baked to PMTiles.** You bake cells to tile57 bundles (through the
  Build Charts dialog or the `tile57` CLI) before OpenCPN can load them — there is no
  live per-view rendering of a raw `.000`. Encrypted (S-63) cells are not supported;
  the source `.000` must be unencrypted.
- **Portrayal fidelity follows tile57.** Anything the engine doesn't yet portray
  correctly won't be correct here either — see tile57's own
  [Known Limitations](https://beetlebugorg.github.io/tile57/limitations).

## Reporting issues

Please file bugs — with the ENC cell name and a screenshot if you can — on the
[GitHub issue tracker](https://github.com/beetlebugorg/tile57-opencpn-plugin/issues).
