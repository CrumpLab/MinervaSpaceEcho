# MINERVA Space Echo

An experimental audio effect (AU / VST3 for macOS, aimed at Ableton Live) that
treats Hintzman's **MINERVA II** multiple-trace memory model as a tape echo. A
Roland Space Echo has one tape loop. This plugin stores every segment of incoming
audio as a separate memory trace, and plays back an *echo*: a blend of all the
traces, each weighted by how similar it is to what you are playing now.

See [`plan.md`](plan.md) for the concept and the staged build plan.

**Status: Stage 1 ("Tape = Memory").** The plugin cuts the input into traces
(1 bar by default, tempo-synced or free). It stores up to 100 of them and
cues memory with each bar you play. It then plays the resulting echo, an
activation-weighted blend of stored bars, during the next bar. With Memory
Capacity = 1 it is exactly a 1-bar tape delay.

## How it behaves

At every trace boundary (e.g. each bar line):

1. The bar just played is **stored** as a trace: its audio, plus a feature
   vector (16 time slots × 24 frequency bands, level-independent) that serves
   as its address.
2. The same bar is the **cue** (probe). Each stored trace's similarity *S* to
   the cue becomes an activation *A = sign(S)·|S|^power*.
3. The **echo** heard during the next bar is Σ *A·trace audio*, normalised.

## Parameters

| Parameter | What it does |
|---|---|
| **Sync** / **Trace Length (Sync)** / **Trace Length (Free)** | Trace length: 1/16 … 4 bars following Live's tempo, or 10 ms … 20 s free-running. Tempo-synced traces align to Live's bar grid. |
| **Memory Capacity** | Max traces (1–1000). When full, the oldest trace is replaced (FIFO). Changing it rebuilds (and clears) memory. |
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

The bottom of the plugin window shows memory fill, how many traces the current
echo uses, its intensity (MINERVA's familiarity signal), the host clock, and a
**Clear Memory** button.

Listening examples: `scripts/render_examples.sh` renders every preset in
`presets/examples/` (CI uploads them as the `example-renders` artifact).

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
