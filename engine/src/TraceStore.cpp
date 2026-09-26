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

    buffers.resize (static_cast<size_t> (numSlots));
    slots.resize (static_cast<size_t> (numSlots));
    const auto perSlot = static_cast<size_t> (slotLen) * static_cast<size_t> (channels);
    for (size_t s = 0; s < slots.size(); ++s)
    {
        buffers[s].reset (new float[perSlot]); // deliberately uninitialised
        slots[s].maxLen = slotLen;
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

void TraceStore::swapBuffers (TraceSlot& a, std::unique_ptr<float[]>& bufA,
                              TraceSlot& b, std::unique_ptr<float[]>& bufB) noexcept
{
    std::swap (bufA, bufB);
    std::swap (a.audio, b.audio);
    std::swap (a.maxLen, b.maxLen);
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
            swapBuffers (mine, buffers[static_cast<size_t> (spares[w])],
                         theirs, old.buffers[static_cast<size_t> (old.spares[w])]);
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
        auto& dst = slots[static_cast<size_t> (to)];
        swapBuffers (dst, buffers[static_cast<size_t> (to)], src, old.buffers[static_cast<size_t> (from)]);
        auto* audio0 = dst.audio[0];
        auto* audio1 = dst.audio[1];
        const auto maxLen = dst.maxLen;
        dst = src;                 // metadata + features
        dst.audio[0] = audio0;     // ...but keep the swapped-in buffer
        dst.audio[1] = audio1;
        dst.maxLen = maxLen;
        order.push_back (to);
    }
}

int TraceStore::appendTrace (const TraceSlot& meta, const float* const* audio, int numCh, int64_t length)
{
    if (full() || freeSlots.empty())
        return -1;
    const int to = freeSlots.back();
    freeSlots.pop_back();
    auto& dst = slots[static_cast<size_t> (to)];
    auto* a0 = dst.audio[0];
    auto* a1 = dst.audio[1];
    const auto maxLen = dst.maxLen;
    dst = meta;
    dst.audio[0] = a0;
    dst.audio[1] = a1;
    dst.maxLen = maxLen;

    const int64_t n = std::min (length, maxLen);
    dst.begin = std::min (dst.begin, n);
    dst.end = std::clamp (dst.end, dst.begin, n);
    for (int c = 0; c < channels; ++c)
        std::memcpy (dst.audio[c], audio[std::min (c, numCh - 1)], static_cast<size_t> (n) * sizeof (float));
    if (dst.serial == 0)
        dst.serial = nextSerial;
    nextSerial = std::max (nextSerial, dst.serial + 1);
    order.push_back (to);
    return to;
}

} // namespace mse
