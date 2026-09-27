# MINERVA Space Echo — Concept & Multi-Stage Build Plan

An experimental audio effect plugin (AU + VST3, macOS, for Ableton Live) that
treats Hintzman's MINERVA II multiple-trace memory model as a tape echo.

> **Status:** reviewed. Decisions are recorded in §9. Licence: open source (AGPLv3,
> following JUCE's open-source licence).

---

## 1. The core analogy

A Roland RE-201 Space Echo records audio onto one tape loop and plays it back
through several heads. MINERVA stores **every** experience as its own trace, and
responds to a cue with an **echo**: a blend of all traces, each weighted by how
similar it is to the cue.

| Space Echo (RE-201) | MINERVA Space Echo |
|---|---|
| Tape loop (one continuous memory) | **Memory matrix**: many traces, one per audio segment |
| Record head writes the input to tape | **Encoding**: each finished segment of input is stored as a new trace |
| Tape wear, dropouts, hiss | **Encoding failure `Lf`** (features/content dropped at storage) and **decay** (traces degrade over time) |
| Repeat Rate (tape speed = delay time) | **Trace length**: 10 ms … 20 s, or 1/16 note … 4 bars when clocked |
| Playback heads (1, 2, 3) | **Retrieval heads**: each head cues memory and plays back an echo |
| Mode selector (head combinations) | **Head configuration** (direct, delayed, iterative recall, top-k voices) |
| Intensity (feedback) | **Feedback**: echo output mixed back into what is recorded next |
| Echo Volume | Echo level / dry-wet mix |
| Bass / Treble in the loop | EQ in the feedback path |
| — | **Memory capacity, full-memory policy, trace locking, activation power** |

### The key sanity check

With **capacity = 1 trace, `Lf` = 0, and the just-stored trace allowed to match
itself**, the echo is exactly the previous segment: **a plain delay with
delay time = trace length**. Everything else in the plugin is what happens as
you move away from that point:

- **More traces:** the echo becomes a weighted *chorus* of every past segment
  that resembles the current one.
- **High activation power:** the chorus narrows to the nearest match (the
  memory "picks" the most similar past bar).
- **Low power:** everything blends into a wash, the memory's "schema" or
  prototype.
- **`Lf` / decay:** holes and wear, like old tape.

This gives us a built-in regression test (the plugin must reproduce a clean
delay in that configuration) and a musically intuitive starting point.

---

## 2. The model, made concrete for audio

### 2.1 What is a trace?

Each trace `i` holds two things:

1. **Content** `a_i`: the actual audio of the segment (stereo, `L` samples).
   This is what gets played back.
2. **Address** `f_i`: a compact feature vector describing the segment. This is
   what similarity is computed on.

Why not compute similarity directly on raw waveforms? Two segments that sound
identical can have near-zero waveform correlation because of phase. Features
make "sounds similar" measurable. (Raw waveform correlation stays available as
an experimental option.)

### 2.2 Feature vector (address)

- The segment is divided into **K time slots** (default 16), regardless of its
  length.
- For each slot, compute **B band energies** (default 24 log-spaced bands, log
  magnitude) from a mono sum.
- Normalize: subtract the trace mean, divide by its std, so values are centred
  around 0 with roughly ±1 range. This is the continuous analogue of MINERVA's
  +1/−1.
- Optional **ternarize** with a dead zone θ: `+1` if above θ, `−1` if below −θ,
  otherwise `0`. This is the classic MINERVA representation.
- **Feature masks** (performance control): *rhythm only* (collapse bands to one
  loudness envelope per slot), *timbre only* (average over slots), or both.
  This mirrors the "lock the rhythm, let pitch wander" idea from the earlier
  chat.
- Later options: chroma (pitch class), spectral flux/onsets, MFCC.

Because features use K slots rather than absolute time, **traces of different
lengths are still comparable**. This matters when the user changes trace length
while memory is full.

### 2.3 Retrieval

Given a probe `p` (features of the cue):

```
S_i = Σ_j p_j · f_ij / N_ij     N_ij = # features where p_j or f_ij is nonzero  (Hintzman 1986)
      (alternative: cosine similarity for continuous features)
A_i = sign(S_i) · |S_i|^POWER   POWER default 3, continuous 1–9
I   = Σ_i A_i                   intensity = familiarity of the cue
echo_audio(t)    = g · Σ_i A_i · a_i(t)
echo_features    = Σ_i A_i · f_i      (used for iterative recall, §4.3)
```

Options to expose:

- **Negative activations:**
  - *subtract*: phase-inverted contribution (true MINERVA)
  - *ignore*: rectify to 0
  - *absolute*
- **Normalization `g`:**
  - `1/Σ|A_i|`: constant loudness
  - `1/max|A_i|`
  - **familiarity-gated**: louder echo for familiar input, faint echo for
    novel input. This is a very MINERVA behaviour.
- **Self-match:** include or exclude the trace that was just stored. Including
  it gives the plain-delay component.
- **Recency weighting:** optional multiplier favouring newer traces, which
  blends between "memory" and "tape".

### 2.4 Cost: this is cheap

A retrieval with 64 traces × 16 slots × 24 bands is about 25k multiply-adds.
Even retrieving every 10 ms, this is a negligible fraction of one core on a
modern Mac. Summing audio content is `#active traces × samples`, which is also
cheap. **Real-time retrieval is not a CPU problem.** The real constraint is
*causality* (§3).

### 2.5 Memory budget

Content storage dominates. Stereo float at 48 kHz is about 384 KB/s.

| Trace length | Capacity | RAM |
|---|---|---|
| 1 bar @120 (2 s) | 64 | ~49 MB |
| 4 bars @120 (8 s) | 64 | ~197 MB |
| 20 s | 32 | ~246 MB |
| 250 ms | 512 | ~49 MB |

**Defaults: trace length = 1 bar at 120 BPM (2 s), capacity = 100 traces**
(≈77 MB at 48 kHz).

Plan: a configurable **memory budget** (default 1 GB). Capacity is derived as
`min(max traces, budget / trace size)`. For example, 100 × 4 bars @120 ≈ 307 MB
fits, but 100 × 20 s ≈ 768 MB only fits under a larger budget. When the budget
limits capacity, the UI shows the **effective capacity**. All memory is preallocated when
settings change, never on the audio thread. Storing content as int16 is an
optional later optimization that halves RAM.

---

## 3. Real-time: how can the live input act as the cue?

The issue: a trace only exists once its segment has finished, so you can't
fully compare a segment you haven't heard yet. There are three cueing
strategies, from simplest to most "live". The plugin should offer all of them
as a **Cue Mode** switch.

### Mode A: Segment cue (delay-like)
At the end of segment `n` (e.g. each bar line):
1. Store segment `n` as a trace.
2. Use segment `n` as the probe and retrieve.
3. Play the resulting echo **during segment `n+1`**, aligned to its start.

This behaves exactly like a delay whose time equals the trace length. Retrieval
runs once per segment. It has zero plugin latency, since the delay *is* the
effect. **This is Stage 1.**

### Mode B: Progressive cue (continuous, within segment)
While segment `n` is still playing, the partial segment so far (slots `1..k`)
is the probe. It is compared against the same slots `1..k` of each trace
(prefix match). Activations update at every slot or hop. The echo plays trace
content at the **current position** (or `+Δ` ahead: a "prediction head").

- The echo follows the input as it unfolds. When you start playing something
  familiar, the matching memories fade up mid-bar.
- Running sums make updates incremental.
- Activation changes are smoothed and crossfaded (≈5–50 ms) to avoid clicks.

### Mode C: Rolling-window cue (unclocked, free-running)
The probe is the last `W` seconds of input, compared against **every offset**
inside every trace (a sliding search). The echo plays from the best-matching
positions. This is closer to concatenative / "audio-mosaic" behaviour, and it
works without a clock.

It is heavier: `traces × offsets × window features`. It runs on a background
thread at a coarser hop with lock-free handoff to the audio thread. It is
feasible on a Mac with reduced features (Stage 4).

### Other cue sources (probe source switch)
- **Input** (default).
- **Sidechain input**: one signal is stored, another cues it. For example, drums
  cue a memory of pads.
- **Echo** (iterative recall, §4.3).
- **Noise / random probe**: free "dreaming" from memory.
- **Frozen probe**: hold a cue and let memory keep answering.

---

## 4. Playback options ("heads")

### 4.1 Blend
The pure MINERVA echo: an activation-weighted sum of trace content. With
clocked, bar-aligned traces this stacks past bars in time, a rhythmic chorus.

### 4.2 Top-k voices (the chorus)
The **k most-activated traces** play as separate voices, each with its own
gain (∝ activation), pan spread, small detune/varispeed and micro-delay. This is
the most "chorus of echoes" setting.

### 4.3 Multiple heads (RE-201-style)
Up to 3–4 heads, each independently configured:
- **Delay head**: echo of segment `n−d` played in segment `n+1` (like heads 1/2/3
  at different tape distances).
- **Iterative head**: head 2 uses head 1's `echo_features` as its probe, head 3
  uses head 2's, and so on. Each generation drifts toward the memory's
  prototype. This is Hintzman's iterative retrieval, and musically each repeat
  gets more "generic".
- **Prediction head** (Mode B): plays trace content ahead of the current
  position.

A **Mode selector** offers preset head combinations, as on the RE-201.

### 4.4 Sampling
Instead of blending, **pick one trace** at random with probability ∝ `|A_i|^POWER`
per segment (or per slot). This gives clean audio with no smear and a
stochastic, generative character.

### 4.5 Length mismatch
If trace length changes while older traces are in memory:
- **Varispeed**: resample to fit, so pitch shifts like changing tape speed.
  Very Space Echo.
- **Truncate / loop**.
- **Time-stretch**: later, with the spectral engine.

### 4.6 De-clicking
Short fades (1–10 ms, configurable) at trace edges, and crossfades whenever the
activation set changes.

---

## 5. Memory management

### 5.1 When to write (encoding gate)
- **Always**: every segment.
- **Level-gated**: skip silent or near-silent segments.
- **Novelty-gated**: store only if max similarity to memory < threshold. The
  memory collects *new* things and stops filling up with repeats.
- **Familiarity-gated**: the inverse, reinforcing what is already known.
- **Probability**: store each segment with probability `p`.
- **Manual capture**: button / MIDI note / automation.
- **Hold / Freeze**: stop writing entirely. Memory is fixed and only playback
  runs, like infinite repeat.

### 5.2 When memory is full
- **FIFO**: overwrite the oldest (tape-like).
- **Random**: overwrite a random unlocked trace.
- **Least used**: overwrite the trace with the lowest accumulated activation
  ("use it or lose it").
- **Weakest**: overwrite the most decayed trace.
- **Consolidate**: if the new trace is very similar to an existing one, *merge*
  (average) instead of adding. Prototypes emerge, a sort of schema formation.
- **Reject**: stop storing when full.

### 5.3 Clamping (locking traces)
"Clamp" = lock a trace in so no full-memory policy can replace it.
- **Clamp individual traces** (pin a good bar). Clamped traces are skipped by
  every replacement policy in §5.2. Optionally they are also exempt from decay
  (a per-trace "exempt from decay" setting).
- **Clamp next capture**: the next stored trace is clamped automatically.
- **Clamp all**: equivalent to Freeze.
- **Clear** memory; **clear unclamped** only.
- If every slot is clamped and memory is full, new traces are rejected.
- **Clamp budget**: optional limit on how many traces may be clamped (e.g.
  25 % of capacity), so memory keeps some room to change.
- *Experimental, later:* an **activation ceiling**, capping how strongly any one
  trace can dominate the echo.

### 5.4 Forgetting
- **`Lf` at encoding**: each feature is zeroed with probability `Lf`.
  Separately, **content dropout** zeroes random slots (gaps) or bands (spectral
  holes) of the stored audio. Tape dropouts.
- **Decay over time**: each segment that passes, every unlocked trace loses
  more features/content and some gain. Optionally a gentle low-pass per
  generation, like tape generation loss.

### 5.5 Feedback
- **Audio feedback** (the Intensity knob): echo output is mixed into the input
  that gets recorded as the next trace, with saturation and EQ in the loop.
  Classic runaway echo is possible (by design, with a limiter for safety).
- **Echo re-encoding**: store the echo itself as a trace. Memory progressively
  feeds on its own reconstructions.

### 5.6 Persistence
- Parameters are saved with the Live set (standard).
- Memory contents are **not** saved with the Live set by default.
- **Save Memory / Load Memory**: write the whole model to a file on disk and
  reload it later:
  - trace audio
  - features
  - per-trace metadata: clamp state, age, use count, decay state, source
    tempo and length
  - model parameters
  
  Format: a folder or zip bundle with a WAV per trace plus a JSON manifest, so
  it is inspectable and editable outside the plugin.
- Option: **embed memory in the Live set** (off by default; can make sets
  large).
- Optional: **preload memory from audio files** (drop a folder of loops to
  seed the memory).

---

## 6. Clocking

- **Synced**: read Live's tempo and bar position from the host (JUCE
  `AudioPlayHead`: BPM, PPQ position, time signature, playing). Trace length
  is set in note values (1/16 … 4 bars). Segment boundaries are aligned to the
  host's bar grid.
- **Free**: trace length in ms (10 ms – 20 s). Boundaries run on an internal
  clock.
- **Transport stopped**: keep the internal clock at the last tempo, or pause
  writing (option).
- Clamp so a trace never exceeds 20 s (or 4 bars) whatever the tempo.

---

## 7. Architecture

```
MinervaSpaceEcho/
  engine/        plain C++20, no framework dependency — the actual model
    FeatureExtractor   (STFT → K×B band features, incremental)
    TraceStore         (preallocated ring of traces: content + features + metadata)
    Retriever          (similarity, activation, intensity, echo features)
    Heads / Voices     (playback, crossfades, varispeed)
    Clock              (host-synced or free segmenting)
    MemoryPolicy       (write gates, full policies, decay, locks)
  plugin/        JUCE wrapper: parameters, state, host clock, editor
  tools/          mse-render (WAV in → WAV out with a parameter preset), mse-testgen
  tests/         unit + regression tests (e.g. "capacity 1 == plain delay")
  .github/workflows/  macOS universal AU/VST3 build + pluginval
```

- **Framework:** JUCE (CMake) builds AU and VST3 plus a Standalone app for
  quick testing. The project is open source under **AGPLv3**, as JUCE's
  open-source licence requires.
- **Engine separate from JUCE:** I can build, test, and render audio examples
  inside my Linux dev container. You audition WAVs before anything goes into
  Live, and the same code ships in the plugin.
- **Real-time rules:** no allocation, locks or I/O on the audio thread.
  Parameter changes that resize memory are applied off-thread and swapped in.
  Heavy searches (Mode C) run on a worker thread.
- **Build/validate:** GitHub Actions macOS runner builds universal binaries
  (arm64 + x86_64) and runs `pluginval`. You can also build locally with
  Xcode + CMake.

---

## 8. Build stages

Each stage ends with something you can **hear**: rendered WAV examples from the
offline tool, and from Stage 1 on, a plugin you can load in Live.

### Stage 0: Scaffolding
- Repo layout, CMake, JUCE via FetchContent, engine library, test framework.
- Offline render CLI (WAV in/out, parameters from a `key = value` preset file
  using the plugin's parameter IDs).
- **Test-audio generator** (deterministic, seeded; nothing copyrighted):
  - synthetic drum loops, including variations and fills
  - chord progressions and a bass line at 120 BPM
  - a melody that repeats with occasional changes (to test recognition)
  - noise bursts and impulses (for timing / delay-accuracy checks)
  - a sudden style change mid-file (to test novelty and forgetting)
- LICENSE (AGPLv3), README.
- Plugin that loads in Live and passes audio through. Generic parameter UI.
- CI: macOS universal AU/VST3 build + pluginval; Linux engine tests.
- **Done when:** you load the pass-through plugin in Live from a CI build.
- **Status:** done. CI green on Linux and macOS (universal AU/VST3, auval,
  pluginval).

### Stage 1: Tape = Memory (segment cue, blend)
- Clock: free (ms) and host-synced (beats/bars) segmenting.
- Trace store: capacity setting (default 100), FIFO, preallocated.
- Features: K×B band energies, normalization, optional ternarize.
- Retrieval: Hintzman similarity, POWER, self-match on/off, negative-activation
  mode, normalization modes.
- Playback: Mode A (echo of segment `n` during `n+1`), blend, edge fades.
- `Lf` (feature dropout), mix, echo level, basic audio feedback + soft clip.
- Regression test: capacity 1 ⇒ exact delay.
- Renders comparing capacity 1 / 8 / 64 and POWER 1 / 3 / 9 on drum loops,
  chords, voice.
- **Done when:** it's a playable "memory delay" in Live.
- **Status:** implemented; awaiting a listen in Live. Built as planned, plus:
  - **Cue gate** (default −60 dBFS). Features are level-independent, so
    without it the noise floor between phrases cued full-level echoes.
  - **Echo level tracking** (0–1, default 1). The echo is scaled by
    cue level ÷ activation-weighted mean level of the retrieved traces. Memory
    picks *what* returns; the input decides *how loud*. Without it, feedback
    never decays with a full memory, because each repeat is rebuilt from
    full-level traces. The factor is exactly 1 at capacity 1, so the delay
    equivalence holds.
  - Memory slots reserve up to 20 s each but are only touched as they fill:
    100 × 1-bar traces use ~77 MB of real RAM.
  - Changing capacity rebuilds memory on the message thread and swaps it in
    without blocking the audio thread. Memory is cleared for now; preserving
    traces across resizes stays in Stage 2.
  - Features come from a 24-band biquad filterbank pooled into 16 slots
    (works for any trace length, 10 ms – 20 s; no FFT needed).
  - Segments with < 50 % coverage (e.g. after a transport jump) cue memory
    but are not stored.

### Stage 2: Memory management
- Write gates: level, novelty, familiarity, probability, manual capture,
  freeze.
- Full policies: FIFO, random, least-used, weakest, consolidate, reject.
- Clamping (locking traces), clamp next capture, clamp budget; clear / clear
  unclamped.
- Content dropout (slot/band), decay over time, generation loss.
- Echo re-encoding (store echoes as traces).
- Memory budget and derived capacity; safe resizing.
- Save Memory / Load Memory (WAV per trace + JSON manifest); optional
  embed-in-Live-set, off by default.
- **Status:** implemented; awaiting a listen in Live.
  - Resizing moves trace buffers between stores instead of copying audio. The
    echo continues through a resize, and the old store lingers for two
    boundaries so anything still playing stays valid.
  - Snapshots are taken without stopping audio: writes pause briefly and a
    seqlock guarantees a consistent copy. The same snapshot feeds Save Memory,
    Save Memory With Set, and `mse-render --save-memory`.
  - Decay scales a trace's *activation* (strength), not its audio. Faded
    memories answer less, and level tracking keeps the echo at the cue's level.
  - Everything runs under AddressSanitizer/UBSan in the test suite, and a
    headless plugin test checks parameter and embedded-memory state round trips.

### Stage 3: Live cueing
- Mode B, progressive prefix cue with smoothed, crossfaded activation updates.
- Prediction head (look ahead `Δ` into matching traces).
- Mode C, rolling-window sliding search on a worker thread (unclocked).
- CPU profiling; SIMD for similarity if needed.
- **Status:** implemented; awaiting a listen in Live.
  - Progressive: re-cued at each of the 16 slot edges with *prefix
    similarity*. Only the slots heard so far are compared, and stored traces
    are re-normalised over that range. The previous bar's Segment echo plays
    until *Progressive Start*.
  - Rolling: every trace now carries a 20 ms frame track (24 band levels)
    alongside its 16-slot address; loaded memories get theirs recomputed.
    Matching uses Pearson correlation over the window (level-independent),
    with O(1) sliding normalisation.
  - Frame matches are only good to ±10 ms, which made the echo jitter early
    and late. The strongest four matches are refined against a 1 ms loudness
    envelope: echo/input envelope correlation went from 0.37 to 0.93 at lag 0.
  - Instead of a worker thread, the search runs incrementally on the audio
    thread with a fixed work budget (~400 multiply-adds per sample). That is
    deterministic and has no races with memory writes; results are checked
    against trace serials. Worst block in the example renders: 1.3 ms of a
    10.7 ms block. Typical cost: Segment ~260x realtime, Progressive ~190x,
    Rolling 30–100x on one core.
  - `mse-render` now reports realtime factor and the slowest block. SIMD not
    needed yet.

### Stage 4: Heads and chorus
- Top-k voices with pan spread / detune / micro-delay.
- Multiple heads: delay heads, iterative-recall heads, per-head level/pan.
- RE-201-style mode selector (preset head combinations).
- Sampling playback mode.
- Familiarity-gated output; intensity as a modulation source (e.g. drives
  filter or feedback).
- Recency weighting; feature masks (rhythm-only / timbre-only).
- Sidechain as alternative cue source; random/frozen probe.
- **Status:** implemented; awaiting a listen in Live.
  - Heads: head 1 = main echo; heads 2–3 are Delay (replay head 1's echo
    from 1 or 2 segments earlier, checked against trace serials) or Iterative
    (cued by the previous head's echo address: activation-weighted mean
    features, re-normalised). Per-head level/pan with per-block ramps.
    Mode Selector: 1, 2, 3, 2+3, 1+2, 1+3, 1+2+3, Iterative 1+2+3, Custom.
  - Voices: the k strongest traces keep the blend's total level; the
    strongest sits in the centre, the others fan out alternately with pan,
    detune (varispeed with linear interpolation) and micro-delay scaled by
    their distance from the centre. With spread/detune/delay at 0 the
    output equals the blend.
  - Sample: one trace drawn with probability proportional to |activation|.
  - Cue sources: Input, Sidechain (a third feature extractor on the
    sidechain; the plugin has an optional sidechain bus), Random, Frozen.
    Random/Frozen skip the cue gate and level tracking.
  - Recency: activation x exp(-recency x age / 4). Feature focus
    (Rhythm/Timbre) collapses bands or slots before comparing; it works with
    progressive prefixes too. (Rolling ignores it.)
  - Familiarity (smoothed strongest activation) drives echo tone and
    feedback. The echo tone filter is bypassed exactly at 20 kHz with no
    modulation.
  - Defaults are unchanged: all earlier tests, including exact-delay
    equivalence, still pass. 80 engine tests + plugin state test, clean
    under ASan/UBSan; heads/voices cost 130–280x realtime.

### Stage 5: Tape character
- Varispeed playback for length mismatch (pitch follows tape speed).
- Wow/flutter, saturation, hiss, feedback-path bass/treble EQ.
- Spring-reverb-style tail (optional).
- **Status:** implemented; awaiting a listen in Live.
  - Every trace records its nominal length (the trace-length setting when it
    was recorded; saved with memory). Length Mismatch = Varispeed plays it at
    recorded / current length speed. Halving the repeat rate on frozen memory
    plays old traces an octave up (verified at 880 Hz from a 440 Hz trace).
    Cut and Loop are the alternatives. Rolling-mode continuations always play
    at their own speed.
  - Wow/flutter modulate the read position of every playing trace
    (fractional reads). Drive is tanh(kx)/k on the echo bus; hiss is
    differenced white noise; feedback EQ is RBJ shelves inside the feedback
    path; the spring is 12 stretched allpass stages in a damped feedback
    loop per channel (43/47 ms), high-passed at 150 Hz, decay set by T60.
  - All defaults are neutral, so every earlier test (including exact-delay
    equivalence) still passes; 88 engine tests + plugin state test, clean
    under ASan/UBSan. Full tape character costs ~70–110x realtime.

### Stage 6: Spectral engine
- Blend traces as **magnitude spectra** (STFT resynthesis). Avoids
  phase-cancellation smear when blending unaligned material.
- Short-frame "grain memory" variant (traces of 20–200 ms, thousands of them).
  Spectral freeze and time-stretch.
- **Status:** implemented; awaiting a listen in Live.
  - Blend Domain = Spectral renders each head by STFT (sqrt-Hann, 75 %
    overlap, ~43 ms frames: 2048 at 48 kHz). Each frame analyses the top
    Spectral Voices memories (current and fading-out playlists, with their
    ramps and gains), sums their magnitudes and gives every bin the phase of
    the loudest memory there. Stereo channels share one packed complex FFT.
    Frames read ahead inside stored traces, so there is no added latency;
    the cost is a ~43 ms fade-in when the echo changes.
  - With one unstretched memory the output reconstructs it (capacity 1 is
    still a delay, to 2e-4). Two anti-phase memories that cancel in the
    waveform domain give full level spectrally (>10x).
  - Length Mismatch = Stretch plays old traces at the new length through a
    phase vocoder (a 2 s, 440 Hz trace played in a 1 s segment stays at
    440 Hz). It uses the spectral path even in the Waveform domain.
  - Spectral Freeze holds the magnitudes and advances each bin's phase by its
    smoothed measured frequency (plus slight jitter), so the drone keeps
    its pitch after memory is cleared.
  - Grain memory: capacity now goes to 4000; Max Active Traces (default 64)
    caps how many traces one blend uses, so 2000 × 15 ms traces run at ~70x
    realtime.
  - 94 engine tests + plugin state test, clean under ASan/UBSan; pluginval
    passes at strictness 10. Spectral presets run at ~14–18x realtime (the
    slowest 512-sample block ~1.5 ms of its 10.7 ms).

### Stage 7: Custom UI and control
- Memory matrix view: traces as rows, live activation bars, intensity meter,
  lock toggles, which traces are playing.
- Drag audio files in to seed memory.
- MIDI control: capture, freeze, clear, head switching.
- Preset system.
- **Status:** implemented; awaiting a listen (and a look) in Live.
  - The engine publishes a *memory view* ~30 times a second through a
    lock-free triple buffer: per trace its serial, activation, what each
    head is playing, level, strength, age, generation, merges, clamp state
    and its quantised address (redrawn only when a trace's features change),
    plus the last cue's address and the echo's content. The audio thread
    never waits for the UI.
  - Editor: the memory matrix (rows = traces, columns = the 384 address
    values), lock / activation / head columns, Heard and Echo spectrograms,
    a familiarity meter, trace inspector (clamp, delete), memory buttons,
    tabbed parameter pages generated from the parameter table (irrelevant
    settings dimmed), status bar. Rendered headless by `mse-ui-snapshot`
    for docs and CI.
  - Commands can address one trace by serial (clamp / unclamp / delete).
  - Drag-and-drop or *Import Audio…*: files are decoded in the background,
    cut at the current trace length and given addresses exactly as live
    recording would (cosine > 0.95 against the engine's own). They join
    memory as the newest traces without stopping audio (built off-thread,
    adopted by buffer swap); optional clamping.
  - MIDI notes from a base note (VST3 / standalone): capture, held freeze and
    spectral freeze, clamp/unclamp/clear, Mode Selector. The memory actions
    are also trigger parameters, for Live's MIDI Map and automation (AU too).
  - Presets: 21 curated factory presets, the 33 listening examples and user
    presets, all in the `mse-render` text format (the parser moved into the
    engine). Presets reset unmentioned settings to defaults, but leave
    session settings (budget, embedding, MIDI) alone.
  - 105 engine tests (11 new) + plugin test (MIDI, presets, import, state),
    clean under ASan/UBSan; pluginval passes at strictness 10.

### Stage 8: Polish and release
- Optimization, parameter smoothing audit, edge cases (sample-rate changes,
  tempo changes mid-segment, offline bounce in Live).
- Signed / notarized builds if you want to share it.

---

## 9. Decisions

| # | Question | Decision |
|---|---|---|
| 1 | Framework | **JUCE + CMake** |
| 2 | Stage 1 cue mode | **Mode A (segment cue)** first. It is the simplest correct version of the model and gives the "capacity 1 = delay" test. Live cueing (Modes B/C) is moved up to **Stage 3**, ahead of heads/chorus, since real-time cueing is the most interesting open question. |
| 3 | "Clamp" | **Lock traces so they can't be replaced** when memory is full (§5.3). Activation ceiling is a later experimental option. |
| 4 | Defaults | **1 bar @ 120 BPM (2 s), 100 traces**; 1 GB memory budget (§2.5) |
| 5 | Test audio | **Generated** by a deterministic test-signal tool (Stage 0) |
| 6 | Memory persistence | **Not saved with the Live set by default.** Explicit Save/Load Memory to file; optional embed (§5.6). |
| 7 | Licence | **Open source, AGPLv3** |
