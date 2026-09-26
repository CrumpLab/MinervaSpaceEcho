// mse-testgen: writes the deterministic test-audio set to a directory.
#include "TestSignals.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

void usage()
{
    std::printf (
        "usage: mse-testgen [options] <output-dir>\n"
        "  --sr <hz>       sample rate (default 48000)\n"
        "  --bpm <bpm>     tempo (default 120)\n"
        "  --bars <n>      length in bars (default 16)\n"
        "  --seed <n>      random seed (default 1)\n"
        "  --pcm24         write 24-bit PCM instead of 32-bit float\n");
}

} // namespace

int main (int argc, char** argv)
{
    mse::testgen::Options opts;
    auto format = mse::WavFormat::Float32;
    std::string outDir;

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
        if (a == "--sr") opts.sampleRate = std::stod (next());
        else if (a == "--bpm") opts.bpm = std::stod (next());
        else if (a == "--bars") opts.bars = std::stoi (next());
        else if (a == "--seed") opts.seed = static_cast<uint32_t> (std::stoul (next()));
        else if (a == "--pcm24") format = mse::WavFormat::Pcm24;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (! a.empty() && a[0] == '-') { std::fprintf (stderr, "unknown option %s\n", a.c_str()); usage(); return 2; }
        else outDir = a;
    }

    if (outDir.empty() || opts.bars < 1 || opts.bpm <= 0.0 || opts.sampleRate <= 0.0)
    {
        usage();
        return 2;
    }

    try
    {
        std::filesystem::create_directories (outDir);
        const auto bpmTag = std::to_string (static_cast<int> (opts.bpm)) + "bpm";
        for (const auto& clip : mse::testgen::generateAll (opts))
        {
            const auto path = (std::filesystem::path (outDir) / (clip.name + "_" + bpmTag + ".wav")).string();
            mse::writeWav (path, clip.audio, format);
            std::printf ("%-40s %6.1f s  %s\n", path.c_str(),
                         clip.audio.numSamples() / opts.sampleRate, clip.description.c_str());
        }
    }
    catch (const std::exception& e)
    {
        std::fprintf (stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
