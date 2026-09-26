#pragma once

namespace mse {

// Snapshot of the host's transport for one processing block.
// Filled from JUCE's AudioPlayHead in the plugin, or synthesised by the
// offline renderer. Values refer to the first sample of the block.
struct HostClock
{
    bool   hasTempo = false;   // false => free-running (no host tempo info)
    double bpm = 120.0;
    double ppqPosition = 0.0;  // position in quarter notes
    int    timeSigNumerator = 4;
    int    timeSigDenominator = 4;
    bool   isPlaying = false;
};

} // namespace mse
