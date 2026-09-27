#!/usr/bin/env bash
# Encodes rendered examples (scripts/render_examples.sh) to MP3 for the manual.
#   usage: scripts/encode_listening.sh [renders-dir] [out-dir]
set -euo pipefail

RENDERS="${1:-renders}"
OUT="${2:-manual/audio}"
mkdir -p "$OUT"
enc() { ffmpeg -loglevel error -y -i "$1" -codec:a libmp3lame -q:a 4 "$2"; }

for wav in "$RENDERS"/*.wav; do
    enc "$wav" "$OUT/$(basename "$wav" .wav).mp3"
done
enc "$RENDERS/input/style_change_120bpm.wav" "$OUT/input_style_change.mp3"
enc "$RENDERS/input/drums_120bpm.wav" "$OUT/input_drums.mp3"
enc "$RENDERS/input/scale_120bpm.wav" "$OUT/input_scale.mp3"
echo "encoded $(ls "$OUT"/*.mp3 | wc -l) files into $OUT"
