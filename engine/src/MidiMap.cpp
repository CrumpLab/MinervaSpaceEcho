#include "mse/MidiMap.h"

#include <cstdio>

namespace mse {

const char* midiActionName (MidiAction a) noexcept
{
    switch (a)
    {
        case MidiAction::Capture:        return "Capture";
        case MidiAction::Freeze:         return "Freeze (hold)";
        case MidiAction::SpectralFreeze: return "Spectral Freeze (hold)";
        case MidiAction::ClampLast:      return "Clamp Last";
        case MidiAction::ClampAll:       return "Clamp All";
        case MidiAction::UnclampAll:     return "Unclamp All";
        case MidiAction::ClearUnclamped: return "Clear Unclamped";
        case MidiAction::ClearAll:       return "Clear All";
        case MidiAction::ModeSelector:   return "Mode Selector";
        case MidiAction::EchoChain:      return "Echo Chain (hold)";
        case MidiAction::None:           break;
    }
    return "";
}

void midiNoteName (int note, char* out, int outSize) noexcept
{
    static const char* const names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    if (note < 0 || note > 127)
    {
        std::snprintf (out, static_cast<size_t> (outSize), "-");
        return;
    }
    std::snprintf (out, static_cast<size_t> (outSize), "%s%d", names[note % 12], note / 12 - 2);
}

} // namespace mse
