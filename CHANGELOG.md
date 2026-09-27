# Changelog

## [0.4.0] - 2026-09-27

### Stage 10: address sets
- Every trace stores five addresses: **Spectrum** (the original), **Pitch
  Class** (chroma, octave-blind), **Pitch** (semitones C2–B5), **Timbre**
  (spectral shape and brightness, aligned to the fundamental) and **Rhythm**
  (onsets in four frequency ranges). The **Address** parameter chooses which
  one memory compares; **Custom** mixes them with five weights (weighted mean
  of the sets' similarities). Sequence context stores every set too.
- With a pitch address the echo chain follows melodies: a scale learned one
  note per bar is walked back in order (with the Spectrum, neighbouring notes
  are 0.98 similar and the walk drifts).
- The memory matrix and side panel show the set being compared, in its own
  layout; new **Address** parameter page.
- Memory files store every set; memory saved by earlier versions gets the new
  sets computed from its audio. Older versions still read new files.
- The default (Spectrum) renders exactly as before: every factory preset and
  earlier listening example is bit-identical.
- Echo Chain switched on after the input has stopped now starts from the echo
  that is playing (it used to wait for input).
- Factory presets *Scale Walker*, *Melody Memory*, *Same Sound*, *Groove
  Recall*; listening examples 38–42 and a scale test signal; an Addresses page
  in the manual.

## [0.3.0] - 2026-09-27

- **Running / Paused** (header button and parameter): pausing leaves memory
  untouched (nothing stored, forgotten or cued), fades the echo out and
  passes the dry signal. Resuming starts cleanly at the next segment.
- **Audition**: listen to a trace on its own (side panel, or Alt-click a
  row), soloed over the plug-in's output, running or paused; **Loop**; and
  **n−1 | n** to hear the segment before it first. Never recorded.
- **Chain Step**: **Sample** (new default) steps the echo chain to one
  answering trace drawn by activation, a random walk through memory that
  follows learned transitions; **Blend** keeps the previous deterministic
  behaviour. Fixes Playback = Sample not affecting the walk.
- **Cue Noise**: noise on every cue (live, chain, iterative, progressive).
- **Habituation**: traces that just answered are briefly less active.
- *Dreaming Sequencer* and example 36 use the sampled walk.

## [0.2.0] - 2026-09-27

### Stage 9: sequential context
After Jamieson and Mewhort (2009), who extended MINERVA to the serial
reaction-time task, and in the spirit of Elman's (1990) context units.

- Every trace also stores the previous segment's address: [n−1 | n]. Trace
  audio is unchanged. Context is recorded always, saved with memory (an
  optional `context` field; older files load without it) and built for
  imported audio from the file's previous piece.
- **Sequence Context** and **Context Cue**: **Predict Next** (the segment just
  played against traces' n−1 halves: memory anticipates the next segment),
  **Match Both** (sequence-sensitive recall, with **Context Weight**) or
  **Current Only**. Works with Segment and Progressive cueing and iterative
  heads.
- **Echo Chain** cue source: each echo's expected next segment cues the next
  echo, so memory walks through learned sequences. **Chain Input** blends in
  the live input (0 = free-running). MIDI base note + 8 holds the chain.
- Memory matrix and side panel show [n−1 | n]; new Sequence page.
- Factory presets *Anticipate*, *Sequence Memory*, *Dreaming Sequencer*;
  examples 34–37; a Sequences page in the manual.
- Off by default: with Sequence Context off, output is unchanged.

## [0.1.0] - 2026-09-27

First release: every stage of `plan.md` is in place. It has been tested
offline and in automated validators (auval, pluginval), but not yet played
in Live, so expect rough edges.

### Stage 8: polish and release
- Blocks larger than the size the host announced (offline bounces) are split
  inside the engine instead of overrunning its buffers.
- Changing the sample rate or channel count keeps memory (resampled or
  mixed down) instead of clearing it.
- Non-finite input (NaN/Inf from a misbehaving plug-in upstream) is replaced
  by silence before it can reach memory.
- Tempo changes mid-segment, transport loops, odd and zero-length blocks and
  determinism of offline renders are covered by tests.
- Parameter smoothing audit: every continuous parameter is switched back
  and forth during a steady tone and checked for clicks (now a test). Wow,
  flutter, spring level, echo tone, tape drive, feedback, the feedback
  shelves and edge fade used to click; they now glide.
- Optimization: vectorised FFT (2.7x faster), phases taken once per bin in
  the spectral engine, a fast path for plain playback and vectorised
  similarity. Most presets run 1.5-2x faster; the spectral ones about 2x
  (26-30x realtime, from 14x).
- macOS installer (.pkg) alongside the zip; optional Developer ID signing
  and notarization in CI; tagged versions become GitHub releases.

### Stage 7: custom UI and control
- Memory matrix editor (traces as rows with their addresses, activations,
  heads and locks), Heard/Echo spectrograms, familiarity meter, trace
  inspector, tabbed parameter pages.
- Drag-and-drop audio import, MIDI note control, trigger parameters, and
  factory, example and user presets.

### Stage 6: spectral engine
- Spectral blending, time-stretch for length mismatch, spectral freeze,
  grain memories of up to 4000 traces.

### Stage 5: tape character
- Varispeed, wow and flutter, tape drive, hiss, feedback bass/treble and a
  spring reverb.

### Stage 4: heads and chorus
- RE-201-style playback heads and mode selector, chorus voices, sampled
  playback, sidechain, random and frozen cues, recency and feature focus.

### Stage 3: live cueing
- Progressive (within the bar) and rolling (no bar grid) cueing.

### Stage 2: memory management
- Freezing, write gates, clamping, full-memory policies, consolidation,
  forgetting, echo re-encoding, and saving and loading memory.

### Stage 1: tape = memory
- Traces stored per segment, cued by the segment just played; the echo is
  the activation-weighted blend. Memory Capacity = 1 is a tape delay.

### Stage 0: scaffolding
- JUCE/CMake project, engine library, tests, offline tools, CI.
