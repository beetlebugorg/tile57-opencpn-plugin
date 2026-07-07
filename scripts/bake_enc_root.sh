#!/usr/bin/env bash
# Bake a parallel ENC_ROOT_TILES tree from an ENC_ROOT: for every <CELL>.000 in the
# source tree, write <CELL>.pmtiles at the SAME relative path under the output tree.
# The output mirrors the ENC_ROOT structure but holds only pmtiles, so the plugin
# (GetFileSearchMask *.pmtiles) loads the baked charts from a clean, source-free tree
# you can point OpenCPN at as a chart directory.
#
# The tiles carry S-57 attributes: `tile57 bake` includes the per-feature pick blob
# by default, so cursor object-query returns each feature's class + attributes.
#
# Idempotent: a cell whose output .pmtiles already exists is skipped, so re-running
# only bakes new/added cells. Runs ncpu/2 bakes in parallel. Portable to macOS
# (bash 3.2, no nproc/mapfile) and Linux.
#
#   usage: bake_enc_root.sh [ENC_ROOT] [ENC_ROOT_TILES] [tile57-binary]
#     ENC_ROOT         source tree of <CELL>/<CELL>.000 (searched recursively)
#     ENC_ROOT_TILES   output tree (default: <ENC_ROOT>_TILES) — mirrors ENC_ROOT,
#                      pmtiles only
#     tile57-binary    the tile57 CLI baker (default: the c-callbacks worktree)
set -u

ROOT="${1:-$HOME/.local/share/chartplotter/NOAA/ENC_ROOT}"; ROOT="${ROOT%/}"
TILES="${2:-${ROOT}_TILES}"; TILES="${TILES%/}"
T57="${3:-${TILE57_BIN:-$HOME/Projects/tile57-c-callbacks/zig-out/bin/tile57}}"

ncpu=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)
JOBS=$(( ncpu / 2 )); [ "$JOBS" -lt 1 ] && JOBS=1

[ -x "$T57" ]  || { echo "tile57 baker not found/executable: $T57" >&2; exit 1; }
[ -d "$ROOT" ] || { echo "ENC_ROOT not found: $ROOT" >&2; exit 1; }

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
FAILLOG="$WORK/failures.log"; : > "$FAILLOG"
export WORK T57 FAILLOG ROOT TILES

total=$(find "$ROOT" -name '*.000' | wc -l | tr -d ' ')
echo "[$(date +%T)] $total cells under $ROOT; baking with $JOBS jobs into $TILES (pmtiles only)"

# Bake one cell (arg) to a scratch bundle, then drop its chart.pmtiles at the cell's
# relative path under $TILES. Inlined into xargs (not an exported function) for
# old-bash portability.
find "$ROOT" -name '*.000' | sort | xargs -P "$JOBS" -I{} bash -c '
  cell="$1"
  rel="${cell#$ROOT/}"                       # path of the .000 relative to ENC_ROOT
  name=$(basename "$cell" .000)
  outdir="$TILES/$(dirname "$rel")"          # mirror the source structure
  out="$outdir/$name.pmtiles"
  [ -f "$out" ] && exit 0
  mkdir -p "$outdir"
  tmp="$WORK/$name"; rm -rf "$tmp"
  if "$T57" bake "$cell" -o "$tmp" >/dev/null 2>&1 && [ -f "$tmp/tiles/chart.pmtiles" ]; then
    cp "$tmp/tiles/chart.pmtiles" "$out"
  else
    echo "$name" >> "$FAILLOG"
  fi
  rm -rf "$tmp"
' _ {}

ok=$(find "$TILES" -name '*.pmtiles' 2>/dev/null | wc -l | tr -d ' ')
fail=$(wc -l < "$FAILLOG" | tr -d ' ')
echo "[$(date +%T)] DONE: $ok pmtiles under $TILES, $fail failures"
[ "$fail" -gt 0 ] && { echo "failed cells:" >&2; cat "$FAILLOG" >&2; }
exit 0
