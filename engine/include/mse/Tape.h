#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace mse {

// Tape character (plan Stage 5): the parts of a Space Echo that are about the
// machine rather than the memory. All real-time safe after prepare().

// Low + high shelf (RBJ) per channel, for the feedback path's bass/treble.
class ShelfEq
{
public:
    void prepare (double sampleRate) noexcept;
    // Recomputes coefficients only when the settings change.
    void set (float bassDb, float trebleDb) noexcept;
    bool isFlat() const noexcept { return flat; }
    float process (int channel, float x) noexcept;
    void reset() noexcept;

private:
    struct Biquad
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float z1[2] {}, z2[2] {};
        float process (int c, float x) noexcept
        {
            const float y = b0 * x + z1[c];
            z1[c] = b1 * x - a1 * y + z2[c];
            z2[c] = b2 * x - a2 * y;
            return y;
        }
    };
    double sr = 48000.0;
    float bass = 0.0f, treble = 0.0f;
    bool flat = true;
    Biquad low, high;
};

// Wow (slow, drifting) and flutter (fast) speed wobble, expressed as a read
// delay in samples (>= 0) for each output sample.
class TapeMotion
{
public:
    void prepare (double sampleRate) noexcept;
    void fill (float* delaySamples, int n, float wow, float flutter) noexcept;

private:
    double sr = 48000.0;
    double wowPhase = 0.0, flutterPhase = 0.0, flutterPhase2 = 0.0;
    double drift = 0.0, driftTarget = 0.0;
    uint64_t rng = 0x5eed;
};

// Spring-reverb-style tail: a dispersive chain of stretched allpass filters
// inside a damped feedback loop, one per channel with slightly different
// lengths. Gives the "drip" and metallic chirp of a spring tank.
class SpringReverb
{
public:
    void prepare (double sampleRate);
    void reset() noexcept;
    void setDecay (float seconds) noexcept;
    float process (int channel, float x) noexcept;

private:
    static constexpr int kStages = 12;
    static constexpr int kStretch = 3; // allpass unit delay: chirp around sr / (2 * kStretch)

    struct Channel
    {
        std::vector<float> delay;
        int writePos = 0;
        float feedbackGain = 0.0f;
        float damp = 0.0f, hp = 0.0f, hpIn = 0.0f, tone = 0.0f;
        std::array<std::array<float, kStretch>, kStages> apX {}, apY {};
        int apPos = 0;
    };
    double sr = 48000.0;
    float decay = 2.0f;
    std::array<Channel, 2> ch;
};

} // namespace mse
