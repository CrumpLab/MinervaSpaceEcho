#!/usr/bin/env bash
# Renders every preset in presets/examples/ over the generated test audio.
#   usage: scripts/render_examples.sh [build-dir] [out-dir]
set -euo pipefail

BUILD="${1:-build}"
OUT="${2:-renders}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

mkdir -p "$OUT/input"
"$BUILD/tools/mse-testgen" "$OUT/input" > /dev/null

for preset in "$ROOT"/presets/examples/*.txt; do
    name="$(basename "$preset" .txt)"
    # Short traces suit the drum loop; everything else uses the style-change
    # file (8 bars of style A, then 8 bars of a different style B).
    input="$OUT/input/style_change_120bpm.wav"
    [[ "$name" == *short_traces* || "$name" == *drums* ]] && input="$OUT/input/drums_120bpm.wav"
    "$BUILD/tools/mse-render" --preset "$preset" --tail 4 "$input" "$OUT/$name.wav"
done
