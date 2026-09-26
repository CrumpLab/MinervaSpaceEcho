# MINERVA Space Echo

An experimental audio effect (AU / VST3 for macOS, aimed at Ableton Live) that
treats Hintzman's **MINERVA II** multiple-trace memory model as a tape echo. A
Roland Space Echo has one tape loop. This plugin stores every segment of incoming
audio as a separate memory trace, and plays back an *echo*: a blend of all the
traces, each weighted by how similar it is to what you are playing now.

See [`plan.md`](plan.md) for the concept and the staged build plan.

**Status: Stage 3 (live cueing).** The plugin cuts the input into traces
(1 bar by default, tempo-synced or free). It stores up to 100 of them and
cues memory with each bar you play. It then plays the resulting echo, an
activation-weighted blend of stored bars, during the next bar. With Memory
Capacity = 1 it is exactly a 1-bar tape delay. Stage 2 adds control over what
memory keeps: freezing, write gates, clamping, full-memory policies,
consolidation, forgetting, echo re-encoding, and saving and loading memory.
Stage 3 lets the live input cue memory *while it plays*: progressively within
the bar, or with no bar grid at all (rolling).

## How it behaves

At every trace boundary (e.g. each bar line):

1. The bar just played is **stored** as a trace: its audio, plus a feature
   vector (16 time slots × 24 frequency bands, level-independent) that serves
   as its address.
2. The same bar is the **cue** (probe). Each stored trace's similarity *S* to
   the cue becomes an activation *A = sign(S)·|S|^power*.
3. The **echo** heard during the next bar is Σ *A·trace audio*, normalised.

That is **Cue Mode = Segment**. Two live modes change *when* memory is cued:

- **Progressive:** from the *Progressive Start* slot (of 16 per bar) on, memory
  is re-cued at every slot with the part of the bar heard so far, compared
  with the same stretch of each stored bar. Bars that began like this one
  play along in step with you, and the echo sharpens as the bar unfolds.
  Before that slot, the previous bar's Segment echo plays.
- **Rolling:** no bar grid. Every *Rolling Interval*, the last *Rolling Window*
  of input is searched for at every position inside every stored trace
  (20 ms frames, then refined to about 1 ms). Memory then plays what
  *followed* the best matches, aligned with now. The search is spread over
  audio blocks with a fixed work budget, so it never spikes the CPU. Traces
  must be longer than the window (the status line warns if not).

## Parameters

| Parameter | What it does |
|---|---|
| **Sync** / **Trace Length (Sync)** / **Trace Length (Free)** | Trace length: 1/16 … 4 bars following Live's tempo, or 10 ms … 20 s free-running. Tempo-synced traces align to Live's bar grid. |
| **Memory Capacity** / **Memory Budget** | Max traces (1–1000) and the audio memory reserved for them (128 MB – 4 GB; with many traces the budget limits the longest trace). Changing either keeps the traces (clamped first, then newest). |
| **When Full** | Which trace makes room: Oldest, Random, Least Used (lowest recent activation), Weakest (most faded), Merge Similar (average the new bar into its closest unclamped trace), or Reject (keep what's there). |
| **Consolidate Above** | Merge a new bar into an existing trace whenever they are at least this similar, even if there is room (1 = off). Prototypes form. |
| **Freeze Memory** | Stop storing and stop decay; memory keeps answering. |
| **Write Mode** / **Capture** | Auto stores every bar that passes the gates; Manual stores only captured bars. Capture (parameter or button) stores the bar in progress, bypassing gates and freeze. |
| **Write Gate** | Bars quieter than this aren't stored (−100 dB = off). |
| **Novelty Gate** / **Novelty Threshold** | Store only novel bars (nothing in memory this similar) or only familiar ones. |
| **Write Probability** | Chance that a bar is stored. |
| **Record Source** | What gets stored: the Input (plus feedback), the Echo (memory feeds on itself; it keeps sustaining after the input stops), or both. |
| **Clamp Incoming** / **Clamp Budget** / **Clamped Don't Decay** | Clamp newly stored bars; the most memory that may be clamped; whether clamped traces are exempt from forgetting. Clamped traces are never replaced. |
| **Tape Dropouts** | At recording, each 1/32 of a trace may drop out. |
| **Forgetting / Segment** / **Fading / Segment** | Each bar, stored traces lose features (weaker recall) and strength (weaker activation). Traces faded below −60 dB are forgotten. |
| **Wear Tone** | Older traces play back duller. |
| **Cue Mode** | Segment (echo of the last bar, like a delay), Progressive (memory follows the bar as it unfolds), or Rolling (unclocked search). |
| **Progressive Start** | Slot (of 16) from which the progressive cue takes over. |
| **Rolling Window** / **Rolling Interval** | Length of the live cue, and how often memory is searched. |
| **Prediction** | Play memory this far ahead of the current position: hear what came next last time. |
| **Cue Smoothing** | Crossfade whenever a live cue changes the echo. |
| **Activation Power** | 1 = every memory contributes (a blurred "schema"); 9 = only the closest memories answer. |
| **Similarity** | Hintzman (MINERVA II's normalised dot product) or Cosine. |
| **Features** / **Ternary Threshold** | Continuous feature values, or classic MINERVA −1/0/+1 values. |
| **Encoding Failure (Lf)** | Probability that each feature of a *stored* trace is lost (tape wear for the address). |
| **Self Match** | On: the bar just played can answer its own cue (the delay-like part). Off: memory answers only with *earlier* bars. |
| **Cue Gate** | Segments quieter than this don't cue memory (stops the noise floor from triggering echoes). |
| **Negative Activations** | Subtract (phase-inverted, true MINERVA), Ignore, or Absolute. |
| **Echo Normalization** | Sum (constant loudness), Max, or Familiarity (echo is quiet when the cue resembles nothing in memory). |
| **Echo Level Tracking** | 1: echo level follows the cue's level, like a tape repeat, so feedback decays. 0: memories return at their own level. |
| **Feedback** | Echo mixed back into what gets recorded (soft-clipped; >1 allowed). |
| **Echo Level** / **Dry Level** / **Output Gain** | Mix. −60 dB = off. |
| **Edge Fade** | Fade length at trace edges and when the echo changes (declicking). |

The bottom of the plugin window shows memory fill and clamping, what happened
to the last bar (stored / merged / gated / rejected / frozen), how many traces
the current echo uses, its intensity (MINERVA's familiarity signal) and the
host clock. Buttons: **Capture**, **Clamp Last**, **Clamp All**, **Unclamp
All**, **Clear Unclamped**, **Clear All**, **Save Memory…**, **Load Memory…**.

**Saving memory.** *Save Memory…* writes a folder containing one WAV per trace
plus `manifest.json` (addresses, clamp state, ages, strengths, and the
parameters in use). *Load Memory…* reloads it, resampling if the sample rate
differs. The folder is plain files, so you can inspect it or build one by
hand. Memory is not saved with the Live set unless **Save Memory With Set** is
on (sets can get large: about 23 MB per minute of stored audio).

Listening examples: `scripts/render_examples.sh` renders every preset in
`presets/examples/` (CI uploads them as the `example-renders` artifact).
Presets can change settings partway through a render with timed lines such as
`@8 freeze = on` or `@2 command = clamp_all`. `mse-render --save-memory DIR` and
`--load-memory DIR` use the same folder format as the plugin.

## Repository layout

| Path | Contents |
|---|---|
| `engine/` | The model: plain C++20, no framework dependency |
| `plugin/` | JUCE wrapper (AU, VST3, Standalone) |
| `tools/` | `mse-testgen` (synthetic test audio) and `mse-render` (offline WAV processing) |
| `tests/` | Catch2 unit tests |
| `presets/` | Parameter presets for `mse-render` (`key = value` lines) |

## Getting a build on your Mac

### Option A: download from CI
Every push builds a universal (Apple Silicon + Intel) AU and VST3 on GitHub
Actions and validates them with `auval` and `pluginval`.

1. Open the repo's **Actions** tab, pick the latest `build` run, and download
   the `MinervaSpaceEcho-macOS` artifact.
2. Unzip it, then copy the plugins into place:
   ```sh
   cp -R "MINERVA Space Echo.component" ~/Library/Audio/Plug-Ins/Components/
   cp -R "MINERVA Space Echo.vst3"      ~/Library/Audio/Plug-Ins/VST3/
   ```
3. The builds are ad-hoc signed (not notarized). Remove the download quarantine
   so macOS will load them:
   ```sh
   xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/"MINERVA Space Echo.component"
   xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/"MINERVA Space Echo.vst3"
   ```
4. In Live: **Settings → Plug-Ins**. Enable "Use Audio Units v2" and/or "Use
   VST3 Plug-In System Folders", then **Rescan**. The plugin appears under
   **CrumpLab**.

### Option B: build locally
Requirements: Xcode command-line tools, CMake ≥ 3.22 (e.g. `brew install cmake ninja`).

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DMSE_COPY_PLUGIN_AFTER_BUILD=ON
cmake --build build
ctest --test-dir build
```

The first configure downloads JUCE and Catch2. `MSE_COPY_PLUGIN_AFTER_BUILD=ON`
installs the AU and VST3 into `~/Library/Audio/Plug-Ins/` automatically.

CMake options: `MSE_BUILD_PLUGIN`, `MSE_BUILD_TOOLS`, `MSE_BUILD_TESTS` (all
default `ON`). Use `-DMSE_BUILD_PLUGIN=OFF` for a fast engine-only build.

## Offline tools

```sh
# Deterministic synthetic test material (drums, chords+bass, melody,
# impulses, a mid-file style change, and a full mix), 16 bars at 120 BPM.
build/tools/mse-testgen test-audio

# Process a file through the engine exactly as a host would.
build/tools/mse-render --preset presets/passthrough.txt --set output_gain_db=-6 \
    --bpm 120 --tail 4 test-audio/full_mix_120bpm.wav out.wav
```

`mse-render` uses the same parameter IDs as the plugin, so a preset sounds the
same offline and in Live.

## Licence

GNU Affero General Public License v3.0 (see [`LICENSE`](LICENSE)), as required
by JUCE's open-source licence.
