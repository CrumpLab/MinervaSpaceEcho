// mse-render: runs a WAV file through the engine offline, block by block,
// exactly as a host would, with a synthesised host clock.
#include "mse/Preset.h"
#include "mse/Wav.h"

#include "mse/EchoEngine.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void usage()
{
    std::printf (
        "usage: mse-render [options] <input.wav> <output.wav>\n"
        "  --preset <file>   parameter preset (key = value lines)\n"
        "  --set key=value   set one parameter (repeatable, applied after --preset)\n"
        "  --bpm <bpm>       host tempo (default 120)\n"
        "  --free            no host tempo (free-running clock)\n"
        "  --block <n>       block size in samples (default 512)\n"
        "  --tail <sec>      append this much silence to hear echo tails (default 0)\n"
        "  --seed <n>        random seed for encoding failure etc. (default 1)\n"
        "  --load-memory <dir>  start with a saved memory (Save Memory folder)\n"
        "  --save-memory <dir>  save the memory at the end of the render\n"
        "  --pcm24           write 24-bit PCM instead of 32-bit float\n"
        "parameters (--set id=value):\n%s", mse::describeParams().c_str());
}

} // namespace

int main (int argc, char** argv)
{
    std::vector<std::string> positional, assignments;
    std::vector<mse::TimedAssignment> timed;
    std::string presetPath;
    double bpm = 120.0, tailSeconds = 0.0;
    bool freeRunning = false;
    int blockSize = 512;
    uint64_t seed = 1;
    std::string loadMemoryDir, saveMemoryDir;
    auto format = mse::WavFormat::Float32;

    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&] () -> std::string {
            if (i + 1 >= argc)
            {
                std::fprintf (stderr, "missing value for %s\n", a.c_str());
                std::exit (2);
            }
            return argv[++i];
        };
        if (a == "--preset") presetPath = next();
        else if (a == "--set") assignments.push_back (next());
        else if (a == "--bpm") bpm = std::stod (next());
        else if (a == "--free") freeRunning = true;
        else if (a == "--block") blockSize = std::stoi (next());
        else if (a == "--tail") tailSeconds = std::stod (next());
        else if (a == "--seed") seed = std::stoull (next());
        else if (a == "--load-memory") loadMemoryDir = next();
        else if (a == "--save-memory") saveMemoryDir = next();
        else if (a == "--pcm24") format = mse::WavFormat::Pcm24;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (! a.empty() && a[0] == '-') { std::fprintf (stderr, "unknown option %s\n", a.c_str()); usage(); return 2; }
        else positional.push_back (a);
    }

    if (positional.size() != 2 || blockSize < 1 || bpm <= 0.0 || tailSeconds < 0.0)
    {
        usage();
        return 2;
    }

    try
    {
        auto values = mse::defaultParamValues();
        if (! presetPath.empty())
            mse::applyPresetFile (values, presetPath, &timed);
        for (const auto& s : assignments)
            mse::applyAssignment (values, s);
        const auto params = mse::paramsFromValues (values);

        auto audio = mse::readWav (positional[0]);
        const int numCh = std::min (audio.numChannels(), mse::EchoEngine::kMaxChannels);
        audio.channels.resize (static_cast<size_t> (numCh));
        const int tail = static_cast<int> (tailSeconds * audio.sampleRate);
        for (auto& ch : audio.channels)
            ch.resize (ch.size() + static_cast<size_t> (tail), 0.0f);

        mse::EchoEngine engine;
        mse::MemoryConfig memory;
        memory.capacity = params.capacity;
        memory.budgetBytes = mse::memoryBudgetBytes (params.memoryBudgetIndex);
        engine.setMemoryConfig (memory);
        if (! loadMemoryDir.empty())
            engine.loadSnapshot (mse::readMemoryFolder (loadMemoryDir));
        engine.setSeed (seed);
        engine.setParams (params);
        engine.prepare (audio.sampleRate, blockSize, numCh);

        mse::HostClock clock;
        clock.hasTempo = ! freeRunning;
        clock.bpm = bpm;
        clock.isPlaying = true;

        std::vector<float*> ptrs (static_cast<size_t> (numCh));
        const int total = audio.numSamples();
        const double samplesPerBar = 4.0 * 60.0 / bpm * audio.sampleRate;
        std::stable_sort (timed.begin(), timed.end(), [] (const auto& a, const auto& b) { return a.bar < b.bar; });
        size_t nextTimed = 0;
        using Clock = std::chrono::steady_clock;
        double processSeconds = 0.0, worstBlock = 0.0;
        for (int pos = 0; pos < total; pos += blockSize)
        {
            // Timed preset lines take effect at the first block at or after their bar.
            bool changed = false;
            while (nextTimed < timed.size() && timed[nextTimed].bar * samplesPerBar <= pos)
            {
                const auto& t = timed[nextTimed++];
                if (t.key == "command")
                    engine.sendCommand (mse::parseCommand (t.value));
                else
                {
                    mse::applyParam (values, t.key, t.value);
                    changed = true;
                }
            }
            if (changed)
                engine.setParams (mse::paramsFromValues (values));

            const int n = std::min (blockSize, total - pos);
            for (int c = 0; c < numCh; ++c)
                ptrs[static_cast<size_t> (c)] = audio.channels[static_cast<size_t> (c)].data() + pos;
            clock.ppqPosition = pos / audio.sampleRate * bpm / 60.0;
            const auto t0 = Clock::now();
            engine.process (ptrs.data(), numCh, n, clock);
            const double dt = std::chrono::duration<double> (Clock::now() - t0).count();
            processSeconds += dt;
            worstBlock = std::max (worstBlock, dt);
        }

        mse::writeWav (positional[1], audio, format);
        if (! saveMemoryDir.empty())
        {
            mse::MemorySnapshot snap;
            if (! engine.takeSnapshot (snap))
                throw std::runtime_error ("could not snapshot memory");
            const auto& specs = mse::paramSpecs();
            for (size_t i = 0; i < specs.size(); ++i)
                snap.params.emplace_back (specs[i].id, values[i]);
            mse::writeMemoryFolder (snap, saveMemoryDir);
            std::printf ("saved %zu traces to %s\n", snap.traces.size(), saveMemoryDir.c_str());
        }
        const auto st = engine.getStats();
        std::printf ("rendered %s -> %s (%.2f s, %d ch, engine %s)\n"
                     "  memory %d/%d traces, %llu stored, trace %.3f s, last echo: %d traces, intensity %.3f\n",
                     positional[0].c_str(), positional[1].c_str(), total / audio.sampleRate, numCh,
                     mse::versionString(), st.tracesStored, st.capacity,
                     static_cast<unsigned long long> (st.segments), st.traceSeconds, st.activeTraces,
                     static_cast<double> (st.intensity));
        std::printf ("  cpu: %.0fx realtime, slowest block %.3f ms (block lasts %.2f ms)\n",
                     total / audio.sampleRate / std::max (1e-9, processSeconds), worstBlock * 1000.0,
                     1000.0 * blockSize / audio.sampleRate);
    }
    catch (const std::exception& e)
    {
        std::fprintf (stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
