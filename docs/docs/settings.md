---
id: settings
title: Settings
sidebar_position: 6
---

# Settings

The plugin honors OpenCPN's own **Vector Chart Display** options — you set them in
**Options → Charts → Vector Chart Display** (and the depth/contour controls), and the
plugin translates them into tile57's *mariner settings* so the S-52 portrayal matches
what you'd expect from a native ENC.

There is no separate settings panel for display options: the plugin reads OpenCPN's
live configuration and re-syncs whenever it changes (it watches OpenCPN's PLIB state
hash, so between changes it costs nothing).

## What maps to what

Some options come from OpenCPN's plugin API getters; the rest are read directly from
OpenCPN's configuration (`[/Settings/GlobalState]`). Together they drive these tile57
mariner fields:

| OpenCPN setting | tile57 mariner field |
| --- | --- |
| Color scheme (Day / Dusk / Night) | `scheme` |
| Safety contour | `safety_contour` |
| Shallow contour | `shallow_contour` |
| Deep contour | `deep_contour` |
| Safety depth | `safety_depth` |
| Two / four depth shades | `four_shade_water` |
| Depth unit | `depth_unit` |
| Display category (Base / Standard / All / Mariner) | `display_base`, `display_standard`, `display_other` |
| Soundings | `display_other` |
| Text | `text_names`, `text_other` |
| Important text only | `text_other` |
| **Light descriptions** | `show_light_descriptions` |
| Extended light sectors | `show_full_sector_lines` |
| SCAMIN on/off | `ignore_scamin` |
| Symbol style (simplified / paper) | `simplified_points` |
| Boundary style (plain / symbolized) | `boundary_style` |
| Show M-object boundaries (meta) | `show_meta_bounds` |
| Data quality (per canvas) | `data_quality` |

Change any of these in OpenCPN and the chart re-portrays with the new settings on the
next redraw. Because a change to depth contours or the palette changes the portrayal,
the plugin invalidates its tile cache when the mariner settings change and re-bakes the
visible tiles.

:::note Light descriptions

If light characteristics (e.g. `Fl R 3s 4.2M`) don't show, check
**Vector Chart Display → Show light descriptions** in OpenCPN — the plugin follows
that toggle.

:::

## Physical sizing

S-52 symbolizes in millimetres, so the plugin scales symbols, text, and line widths to
your display's true physical pixel density (from OpenCPN's reported display size).
Calibration and cull tuning are available through the
[environment knobs](./rendering.md#environment-knobs) (`TILE57_SIZE`, `TILE57_CALIB`,
`TILE57_DECLUTTER`) if a display reports its size inaccurately.
