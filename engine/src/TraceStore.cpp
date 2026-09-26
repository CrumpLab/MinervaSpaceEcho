#include "mse/TraceStore.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mse {

TraceStore::TraceStore (const MemoryConfig& config, double sampleRate, int numChannels)
    : cfg (config),
      rate (sampleRate),
      cap (std::max (1, config.capacity)),
      channels (std::clamp (numChannels, 1, 2))
{
    const int numSlots = cap + kSpares;
    const double bytesPerSample = sizeof (float) * static_cast<double> (channels);
    const double byBudget = config.budgetBytes / (bytesPerSample * numSlots);
    const double byLength = config.maxTraceSeconds * sampleRate;
    slotLen = std::max<int64_t> (64, static_cast<int64_t> (std::min (byBudget, byLength)));

    slotFrames = static_cast<int> (slotLen / frameHop (sampleRate)) + 2;

    buffers.resize (static_cast<size_t> (numSlots));
    frameBuffers.resize (static_cast<size_t> (numSlots));
    slots.resize (static_cast<size_t> (numSlots));
    const auto perSlot = static_cast<size_t> (slotLen) * static_cast<size_t> (channels);
    const auto framesPerSlotFloats = static_cast<size_t> (slotFrames) * kBands;
    for (size_t s = 0; s < slots.size(); ++s)
    {
        buffers[s].reset (new float[perSlot]); // deliberately uninitialised
        frameBuffers[s].reset (new float[framesPerSlotFloats]);
        slots[s].maxLen = slotLen;
        slots[s].frames = frameBuffers[s].get();
        slots[s].maxFrames = slotFrames;
        for (int c = 0; c < channels; ++c)
            slots[s].audio[c] = buffers[s].get() + static_cast<size_t> (c) * static_cast<size_t> (slotLen);
    }

    order.reserve (static_cast<size_t> (cap));
    freeSlots.reserve (static_cast<size_t> (numSlots));
    clear();
}

int TraceStore::positionOf (int slotIndex) const noexcept
{
    for (size_t i = 0; i < order.size(); ++i)
        if (order[i] == slotIndex)
            return static_cast<int> (i);
    return -1;
}

int TraceStore::clampedCount() const noexcept
{
    int n = 0;
    for (int s : order)
        n += slots[static_cast<size_t> (s)].clamped ? 1 : 0;
    return n;
}

void TraceStore::clear() noexcept
{
    order.clear();
    freeSlots.clear();
    for (int s = cap + kSpares - 1; s >= kSpares; --s)
        freeSlots.push_back (s);
    for (int w = 0; w < kSpares; ++w)
        spares[w] = w;
    for (auto& s : slots)
        s.resetMeta();
}

void TraceStore::clearUnclamped() noexcept
{
    for (int i = size() - 1; i >= 0; --i)
        if (! slots[static_cast<size_t> (order[static_cast<size_t> (i)])].clamped)
            removeAt (i);
}

int TraceStore::commitSpare (int which) noexcept
{
    if (full() || freeSlots.empty())
        return -1;
    const int stored = spares[which];
    slots[static_cast<size_t> (stored)].serial = nextSerial++;
    order.push_back (stored);

    spares[which] = freeSlots.back();
    freeSlots.pop_back();
    slots[static_cast<size_t> (spares[which])].resetMeta();
    return stored;
}

void TraceStore::removeAt (int position) noexcept
{
    const int s = order[static_cast<size_t> (position)];
    order.erase (order.begin() + position);
    slots[static_cast<size_t> (s)].resetMeta();
    freeSlots.push_back (s);
}

void TraceStore::swapBuffers (int mine, TraceStore& other, int theirs) noexcept
{
    auto& a = slots[static_cast<size_t> (mine)];
    auto& b = other.slots[static_cast<size_t> (theirs)];
    std::swap (buffers[static_cast<size_t> (mine)], other.buffers[static_cast<size_t> (theirs)]);
    std::swap (frameBuffers[static_cast<size_t> (mine)], other.frameBuffers[static_cast<size_t> (theirs)]);
    std::swap (a.audio, b.audio);
    std::swap (a.maxLen, b.maxLen);
    std::swap (a.frames, b.frames);
    std::swap (a.maxFrames, b.maxFrames);
}

void TraceStore::copyMeta (int to, const TraceSlot& src) noexcept
{
    auto& dst = slots[static_cast<size_t> (to)];
    auto* a0 = dst.audio[0];
    auto* a1 = dst.audio[1];
    const auto maxLen = dst.maxLen;
    auto* frames = dst.frames;
    const auto maxFrames = dst.maxFrames;
    dst = src;
    dst.audio[0] = a0;
    dst.audio[1] = a1;
    dst.maxLen = maxLen;
    dst.frames = frames;
    dst.maxFrames = maxFrames;
}

void TraceStore::adoptFrom (TraceStore& old, bool keepTraces) noexcept
{
    // Spares: keep the in-progress recordings (and their positions).
    for (int w = 0; w < kSpares; ++w)
    {
        auto& mine = slots[static_cast<size_t> (spares[w])];
        auto& theirs = old.slots[static_cast<size_t> (old.spares[w])];
        if (theirs.maxLen >= 1 && channels == old.channels)
        {
            swapBuffers (spares[w], old, old.spares[w]);
            mine.begin = theirs.begin;
            mine.end = theirs.end;
        }
    }

    nextSerial = std::max (nextSerial, old.nextSerial);
    if (! keepTraces || channels != old.channels)
        return;

    // Choose what survives: clamped first (newest first), then unclamped newest.
    // `mergeCount` < 0 is used as a temporary "keep" mark on old slots.
    int budget = cap - size();
    for (int pass = 0; pass < 2 && budget > 0; ++pass)
        for (int i = old.size() - 1; i >= 0 && budget > 0; --i)
        {
            auto& s = old.slots[static_cast<size_t> (old.order[static_cast<size_t> (i)])];
            if (s.clamped == (pass == 0) && s.mergeCount > 0)
            {
                s.mergeCount = -s.mergeCount;
                --budget;
            }
        }

    // Move the kept traces in chronological order.
    for (int i = 0; i < old.size(); ++i)
    {
        const int from = old.order[static_cast<size_t> (i)];
        auto& src = old.slots[static_cast<size_t> (from)];
        if (src.mergeCount > 0)
            continue;
        src.mergeCount = -src.mergeCount;

        const int to = freeSlots.back();
        freeSlots.pop_back();
        swapBuffers (to, old, from); // the trace's buffers move here
        copyMeta (to, src);          // metadata + features
        order.push_back (to);
    }
}

int TraceStore::appendTrace (const TraceSlot& meta, const float* const* audio, int numCh, int64_t length)
{
    if (full() || freeSlots.empty())
        return -1;
    const int to = freeSlots.back();
    freeSlots.pop_back();
    copyMeta (to, meta);
    auto& dst = slots[static_cast<size_t> (to)];

    const int64_t n = std::min (length, dst.maxLen);
    dst.begin = std::min (dst.begin, n);
    dst.end = std::clamp (dst.end, dst.begin, n);
    for (int c = 0; c < channels; ++c)
        std::memcpy (dst.audio[c], audio[std::min (c, numCh - 1)], static_cast<size_t> (n) * sizeof (float));

    // The frame track is derived from the audio.
    std::vector<float> mono (static_cast<size_t> (dst.end), 0.0f);
    for (int c = 0; c < channels; ++c)
        for (int64_t t = dst.begin; t < dst.end; ++t)
            mono[static_cast<size_t> (t)] += dst.audio[c][t] / static_cast<float> (channels);
    FeatureExtractor::computeFrames (mono.data(), dst.begin, dst.end, rate, dst.frames, dst.maxFrames,
                                     dst.frameBegin, dst.frameEnd);

    if (dst.serial == 0)
        dst.serial = nextSerial;
    nextSerial = std::max (nextSerial, dst.serial + 1);
    order.push_back (to);
    return to;
}

} // namespace mse
