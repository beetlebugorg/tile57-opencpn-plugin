#!/usr/bin/env bash
# Bake a <CELL>.pmtiles next to every <CELL>.000 in an ENC_ROOT, so the tile57
# OpenCPN plugin can load the baked charts in place alongside the source cells.
#
# Idempotent: a cell whose <CELL>.pmtiles already exists is skipped, so re-running
# only bakes new/added cells. Runs ncpu/2 bakes in parallel.
# Portable to macOS (bash 3.2, no nproc/mapfile) and Linux.
#
#   usage: bake_enc_root.sh [ENC_ROOT] [tile57-binary]
#     ENC_ROOT        directory holding <CELL>/<CELL>.000 (searched recursively)
#     tile57-binary   the tile57 CLI baker (default: the c-callbacks worktree)
set -u

ROOT="${1:-$HOME/.local/share/chartplotter/NOAA/ENC_ROOT}"
T57="${2:-${TILE57_BIN:-$HOME/Projects/tile57-c-callbacks/zig-out/bin/tile57}}"

ncpu=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)
JOBS=$(( ncpu / 2 )); [ "$JOBS" -lt 1 ] && JOBS=1

[ -x "$T57" ]  || { echo "tile57 baker not found/executable: $T57" >&2; exit 1; }
[ -d "$ROOT" ] || { echo "ENC_ROOT not found: $ROOT" >&2; exit 1; }

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
FAILLOG="$WORK/failures.log"; : > "$FAILLOG"
export WORK T57 FAILLOG

total=$(find "$ROOT" -name '*.000' | wc -l | tr -d ' ')
echo "[$(date +%T)] $total cells under $ROOT; baking with $JOBS jobs (pmtiles placed next to each .000)"

# Bake one cell (arg) to a scratch bundle, then drop chart.pmtiles beside its
# .000. Inlined into xargs (not an exported function) for old-bash portability.
find "$ROOT" -name '*.000' | sort | xargs -P "$JOBS" -I{} bash -c '
  cell="$1"
  dir=$(dirname "$cell"); name=$(basename "$cell" .000)
  out="$dir/$name.pmtiles"
  [ -f "$out" ] && exit 0
  tmp="$WORK/$name"; rm -rf "$tmp"
  if "$T57" bake "$cell" -o "$tmp" >/dev/null 2>&1 && [ -f "$tmp/tiles/chart.pmtiles" ]; then
    cp "$tmp/tiles/chart.pmtiles" "$out"
  else
    echo "$name" >> "$FAILLOG"
  fi
  rm -rf "$tmp"
' _ {}

ok=$(find "$ROOT" -name '*.pmtiles' | wc -l | tr -d ' ')
fail=$(wc -l < "$FAILLOG" | tr -d ' ')
echo "[$(date +%T)] DONE: $ok pmtiles under $ROOT, $fail failures"
[ "$fail" -gt 0 ] && { echo "failed cells:" >&2; cat "$FAILLOG" >&2; }
exit 0
