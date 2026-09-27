#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace mse {

// In-place iterative radix-2 complex FFT (power-of-two sizes).
class Fft
{
public:
    void prepare (int size);
    int size() const noexcept { return n; }
    void forward (float* re, float* im) const noexcept { transform (re, im, false); }
    void inverse (float* re, float* im) const noexcept { transform (re, im, true); } // unscaled

private:
    void transform (float* re, float* im, bool inverse) const noexcept;
    int n = 0;
    std::vector<int> bitrev;
    std::vector<float> twRe, twImFwd, twImInv; // per-stage twiddles, contiguous
};

// One memory feeding a spectral frame (plan Stage 6).
struct SpectralSource
{
    const float* audio[2];
    int64_t begin, end;   // valid trace samples
    double start;         // trace position of the frame's first sample
    double rate;          // trace samples per output sample (!= 1: time-stretched)
    float gain[2];        // per output channel (signs ignored)
};

// Short-time Fourier resynthesis of several memories at once (sqrt-Hann
// windows, 75 % overlap). Magnitudes are summed with their gains; each bin
// takes the phase of the memory that is loudest there, so unaligned memories
// don't comb-filter. Stretched sources get phase-vocoder phases (pitch is
// kept). With one unstretched source the output reconstructs it exactly.
class SpectralRenderer
{
public:
    void prepare (double sampleRate, int maxChannels);
    void reset() noexcept;
    int frameSize() const noexcept { return fft.size(); }
    int hop() const noexcept { return fft.size() / 4; }

    // Synthesises one windowed frame per channel into out[c][0..frameSize).
    // With `freeze`, the last spectrum is held and its phases keep turning.
    void render (const SpectralSource* sources, int numSources, int channels, bool freeze, float* const* out) noexcept;

private:
    // Windowed frame of both channels packed into one complex FFT
    // (left = real part, right = imaginary part), transformed.
    void readFrame (const SpectralSource& s, int channels, double start) noexcept;
    // Channel c's spectrum at bin k, unpacked from the packed transform.
    void unpack (int k, int c, float& xr, float& xi) const noexcept;

    Fft fft;
    std::vector<float> window;             // sqrt-Hann
    std::vector<float> re, im, re2, im2;
    std::array<std::vector<float>, 2> mag, bestMag, bestRe, bestIm, prevRe, prevIm, bestHop, unitRe, unitIm;
    std::array<std::vector<unsigned char>, 2> bestStretched;
    std::array<std::vector<float>, 2> outPhase, frozenMag, binFreq; // binFreq: rad/sample of the last frames
    std::array<bool, 2> haveFrozen {};
    uint64_t rng = 0x51ec7a1;
};

} // namespace mse
