#include "mse/TraceStore.h"

#include <algorithm>
#include <cmath>

namespace mse {

TraceStore::TraceStore (const MemoryConfig& config, double sampleRate, int numChannels)
    : cfg (config),
      cap (std::max (1, config.capacity)),
      channels (std::clamp (numChannels, 1, 2))
{
    const double bytesPerSample = sizeof (float) * static_cast<double> (channels);
    const double byBudget = config.budgetBytes / (bytesPerSample * (cap + 1));
    const double byLength = config.maxTraceSeconds * sampleRate;
    slotLen = std::max<int64_t> (64, static_cast<int64_t> (std::min (byBudget, byLength)));

    const auto perSlot = static_cast<size_t> (slotLen) * static_cast<size_t> (channels);
    storage.reset (new float[perSlot * static_cast<size_t> (cap + 1)]); // deliberately uninitialised

    slots.resize (static_cast<size_t> (cap + 1));
    for (size_t s = 0; s < slots.size(); ++s)
        for (int c = 0; c < channels; ++c)
            slots[s].audio[c] = storage.get() + s * perSlot + static_cast<size_t> (c) * static_cast<size_t> (slotLen);

    fifo.assign (static_cast<size_t> (cap), -1);
    freeSlots.reserve (static_cast<size_t> (cap + 1));
    clear();
}

void TraceStore::clear() noexcept
{
    freeSlots.clear();
    for (int s = cap; s >= 1; --s)
        freeSlots.push_back (s); // capacity was reserved: no allocation
    spare = 0;
    head = count = 0;
    for (auto& s : slots)
        s.begin = s.end = 0;
}

int TraceStore::commitSpare() noexcept
{
    auto& committed = slots[static_cast<size_t> (spare)];
    committed.serial = nextSerial++;
    const int stored = spare;

    if (count == cap)
    {
        // FIFO: evict the oldest; its slot becomes the new spare.
        const int evicted = fifo[static_cast<size_t> (head)];
        fifo[static_cast<size_t> (head)] = stored;
        head = (head + 1) % cap;
        spare = evicted;
    }
    else
    {
        fifo[static_cast<size_t> ((head + count) % cap)] = stored;
        ++count;
        spare = freeSlots.back();
        freeSlots.pop_back();
    }

    auto& s = slots[static_cast<size_t> (spare)];
    s.begin = s.end = 0;
    return stored;
}

} // namespace mse
