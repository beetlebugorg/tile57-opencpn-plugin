#!/usr/bin/env bash
# Bake a <CELL>.pmtiles next to every <CELL>.000 in an ENC_ROOT, so the tile57
# OpenCPN plugin can load the baked charts in place alongside the source cells.
#
# Idempotent: a cell whose <CELL>.pmtiles already exists is skipped, so re-running
# only bakes new/added cells. Runs nproc/2 bakes in parallel.
#
#   usage: bake_enc_root.sh [ENC_ROOT] [tile57-binary]
#     ENC_ROOT        directory holding <CELL>/<CELL>.000 (searched recursively)
#     tile57-binary   the tile57 CLI baker (default: the c-callbacks worktree)
set -u

ROOT="${1:-$HOME/.local/share/chartplotter/NOAA/ENC_ROOT}"
T57="${2:-${TILE57_BIN:-$HOME/Projects/tile57-c-callbacks/zig-out/bin/tile57}}"
JOBS=$(( $(nproc) / 2 )); [ "$JOBS" -lt 1 ] && JOBS=1

[ -x "$T57" ]  || { echo "tile57 baker not found/executable: $T57" >&2; exit 1; }
[ -d "$ROOT" ] || { echo "ENC_ROOT not found: $ROOT" >&2; exit 1; }

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
FAILLOG="$WORK/failures.log"; : > "$FAILLOG"

mapfile -t cells < <(find "$ROOT" -name '*.000' | sort)
echo "[$(date +%T)] ${#cells[@]} cells under $ROOT; baking with $JOBS jobs (pmtiles placed next to each .000)"

# Bake one cell to a scratch bundle, then drop chart.pmtiles beside its .000.
bake_one() {
  local cell="$1" dir name out tmp
  dir=$(dirname "$cell"); name=$(basename "$cell" .000)
  out="$dir/$name.pmtiles"
  [ -f "$out" ] && return 0
  tmp="$WORK/$name"; rm -rf "$tmp"
  if "$T57" bake "$cell" -o "$tmp" >/dev/null 2>&1 && [ -f "$tmp/tiles/chart.pmtiles" ]; then
    cp "$tmp/tiles/chart.pmtiles" "$out"
  else
    echo "$name" >> "$FAILLOG"
  fi
  rm -rf "$tmp"
}
export -f bake_one
export WORK T57 FAILLOG

printf '%s\n' "${cells[@]}" | xargs -P "$JOBS" -I{} bash -c 'bake_one "$@"' _ {}

ok=$(find "$ROOT" -name '*.pmtiles' | wc -l)
fail=$(wc -l < "$FAILLOG")
echo "[$(date +%T)] DONE: $ok pmtiles under $ROOT, $fail failures"
[ "$fail" -gt 0 ] && { echo "failed cells:"; cat "$FAILLOG" >&2; }
exit 0
