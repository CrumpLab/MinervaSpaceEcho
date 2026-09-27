#pragma once

namespace mse {

// MIDI note control (plan Stage 7). Notes are counted from a base note
// (default 36, C1 in Live):
//   +0 Capture          +1 Freeze (held)     +2 Spectral Freeze (held)
//   +3 Clamp Last       +4 Clamp All         +5 Unclamp All
//   +6 Clear Unclamped  +7 Clear All
//   +12 .. +20 Mode Selector: Custom, 1, 2, 3, 2+3, 1+2, 1+3, 1+2+3, Iterative
enum class MidiAction
{
    None,
    Capture,
    Freeze,          // held: on while the note is down
    SpectralFreeze,  // held
    ClampLast,
    ClampAll,
    UnclampAll,
    ClearUnclamped,
    ClearAll,
    ModeSelector,    // `value` is the Mode Selector choice index
};

struct MidiMapping
{
    MidiAction action = MidiAction::None;
    int value = 0;
};

constexpr int kMidiModeOffset = 12;
constexpr int kMidiModeCount = 9;

constexpr MidiMapping midiMappingFor (int note, int baseNote) noexcept
{
    const int k = note - baseNote;
    switch (k)
    {
        case 0: return { MidiAction::Capture, 0 };
        case 1: return { MidiAction::Freeze, 0 };
        case 2: return { MidiAction::SpectralFreeze, 0 };
        case 3: return { MidiAction::ClampLast, 0 };
        case 4: return { MidiAction::ClampAll, 0 };
        case 5: return { MidiAction::UnclampAll, 0 };
        case 6: return { MidiAction::ClearUnclamped, 0 };
        case 7: return { MidiAction::ClearAll, 0 };
        default: break;
    }
    if (k >= kMidiModeOffset && k < kMidiModeOffset + kMidiModeCount)
        return { MidiAction::ModeSelector, k - kMidiModeOffset };
    return {};
}

// Short label for the note (for the UI's MIDI legend).
const char* midiActionName (MidiAction a) noexcept;

// Note name as Live shows it (60 = C3).
void midiNoteName (int note, char* out, int outSize) noexcept;

} // namespace mse
