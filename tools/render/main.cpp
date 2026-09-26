// mse-render: runs a WAV file through the engine offline, block by block,
// exactly as a host would, with a synthesised host clock.
#include "Preset.h"
#include "Wav.h"

#include "mse/EchoEngine.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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
        "  --seed <n>        random seed for encoding failure (default 1)\n"
        "  --pcm24           write 24-bit PCM instead of 32-bit float\n"
        "parameters (--set id=value):\n%s", mse::tools::describeParams().c_str());
}

} // namespace

int main (int argc, char** argv)
{
    std::vector<std::string> positional, assignments;
    std::string presetPath;
    double bpm = 120.0, tailSeconds = 0.0;
    bool freeRunning = false;
    int blockSize = 512;
    uint64_t seed = 1;
    auto format = mse::tools::WavFormat::Float32;

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
        else if (a == "--pcm24") format = mse::tools::WavFormat::Pcm24;
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
            mse::tools::applyPresetFile (values, presetPath);
        for (const auto& s : assignments)
            mse::tools::applyAssignment (values, s);
        const auto params = mse::paramsFromValues (values);

        auto audio = mse::tools::readWav (positional[0]);
        const int numCh = std::min (audio.numChannels(), mse::EchoEngine::kMaxChannels);
        audio.channels.resize (static_cast<size_t> (numCh));
        const int tail = static_cast<int> (tailSeconds * audio.sampleRate);
        for (auto& ch : audio.channels)
            ch.resize (ch.size() + static_cast<size_t> (tail), 0.0f);

        mse::EchoEngine engine;
        mse::MemoryConfig memory;
        memory.capacity = params.capacity;
        engine.setMemoryConfig (memory);
        engine.setSeed (seed);
        engine.setParams (params);
        engine.prepare (audio.sampleRate, blockSize, numCh);

        mse::HostClock clock;
        clock.hasTempo = ! freeRunning;
        clock.bpm = bpm;
        clock.isPlaying = true;

        std::vector<float*> ptrs (static_cast<size_t> (numCh));
        const int total = audio.numSamples();
        for (int pos = 0; pos < total; pos += blockSize)
        {
            const int n = std::min (blockSize, total - pos);
            for (int c = 0; c < numCh; ++c)
                ptrs[static_cast<size_t> (c)] = audio.channels[static_cast<size_t> (c)].data() + pos;
            clock.ppqPosition = pos / audio.sampleRate * bpm / 60.0;
            engine.process (ptrs.data(), numCh, n, clock);
        }

        mse::tools::writeWav (positional[1], audio, format);
        const auto st = engine.getStats();
        std::printf ("rendered %s -> %s (%.2f s, %d ch, engine %s)\n"
                     "  memory %d/%d traces, %llu stored, trace %.3f s, last echo: %d traces, intensity %.3f\n",
                     positional[0].c_str(), positional[1].c_str(), total / audio.sampleRate, numCh,
                     mse::versionString(), st.tracesStored, st.capacity,
                     static_cast<unsigned long long> (st.segments), st.traceSeconds, st.activeTraces,
                     static_cast<double> (st.intensity));
    }
    catch (const std::exception& e)
    {
        std::fprintf (stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
