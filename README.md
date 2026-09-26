# MINERVA Space Echo

An experimental audio effect (AU / VST3 for macOS, aimed at Ableton Live) that
treats Hintzman's **MINERVA II** multiple-trace memory model as a tape echo. A
Roland Space Echo has one tape loop. This plugin stores every segment of incoming
audio as a separate memory trace, and plays back an *echo*: a blend of all the
traces, each weighted by how similar it is to what you are playing now.

See [`plan.md`](plan.md) for the concept and the staged build plan.

**Status: Stage 0 (scaffolding).** The plugin currently passes audio through
with an output-gain control. It shows the host tempo / bar position it
receives, to confirm clock sync inside Live.

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
