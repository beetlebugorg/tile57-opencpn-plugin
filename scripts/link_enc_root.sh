#!/usr/bin/env bash
# Create a parallel <ENC_ROOT>_CELLS tree of *.t57 symlinks, one per <CELL>.000, so
# the tile57 plugin renders the LIVE cells (with real M_COVR coverage → correct
# quilting) instead of the baked pmtiles.
#
# .t57 is a plugin-owned extension the OpenCPN core S-57 engine ignores, so the
# plugin claims these cells (it can't take .000 away from the built-in engine). The
# symlink points at the real .000, whose .001.. update chain the plugin reads from
# the source directory — so nothing else needs linking.
#
# Switch renderers by which directory you add as a chart folder in OpenCPN:
#   <ENC_ROOT>_CELLS  -> live cells (this script)
#   <ENC_ROOT>_TILES  -> baked pmtiles (bake_enc_root.sh)
#
# Idempotent; portable to macOS (bash 3.2) and Linux.
#
#   usage: link_enc_root.sh [ENC_ROOT] [ENC_ROOT_CELLS]
set -u

ROOT="${1:-$HOME/.local/share/chartplotter/NOAA/ENC_ROOT}"
[ -d "$ROOT" ] || { echo "ENC_ROOT not found: $ROOT" >&2; exit 1; }
ROOT="$(cd "$ROOT" && pwd)"                 # absolutize so the links resolve anywhere
LINKS="${2:-${ROOT}_CELLS}"; LINKS="${LINKS%/}"

total=$(find "$ROOT" -name '*.000' | wc -l | tr -d ' ')
echo "[$(date +%T)] linking $total cells from $ROOT into $LINKS (*.t57)"

find "$ROOT" -name '*.000' | while IFS= read -r cell; do
  rel="${cell#$ROOT/}"
  name=$(basename "$cell" .000)
  outdir="$LINKS/$(dirname "$rel")"
  mkdir -p "$outdir"
  ln -sf "$cell" "$outdir/$name.t57"        # $cell is absolute -> link resolves anywhere
done

echo "[$(date +%T)] DONE: $(find "$LINKS" -name '*.t57' 2>/dev/null | wc -l | tr -d ' ') links under $LINKS"
