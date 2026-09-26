#include "mse/Wav.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace mse {

namespace {

uint16_t readU16 (const uint8_t* p) { return static_cast<uint16_t> (p[0] | (p[1] << 8)); }
uint32_t readU32 (const uint8_t* p)
{
    return static_cast<uint32_t> (p[0]) | (static_cast<uint32_t> (p[1]) << 8)
         | (static_cast<uint32_t> (p[2]) << 16) | (static_cast<uint32_t> (p[3]) << 24);
}

void putU16 (std::vector<uint8_t>& v, uint16_t x)
{
    v.push_back (static_cast<uint8_t> (x & 0xff));
    v.push_back (static_cast<uint8_t> (x >> 8));
}
void putU32 (std::vector<uint8_t>& v, uint32_t x)
{
    for (int i = 0; i < 4; ++i)
        v.push_back (static_cast<uint8_t> ((x >> (8 * i)) & 0xff));
}
void putTag (std::vector<uint8_t>& v, const char* tag) { v.insert (v.end(), tag, tag + 4); }

constexpr uint16_t kFormatPcm = 1;
constexpr uint16_t kFormatFloat = 3;
constexpr uint16_t kFormatExtensible = 0xFFFE;

} // namespace

AudioBuffer readWav (const std::string& path)
{
    std::ifstream in (path, std::ios::binary);
    if (! in)
        throw std::runtime_error ("cannot open " + path);

    std::vector<uint8_t> data ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
    if (data.size() < 12 || std::memcmp (data.data(), "RIFF", 4) != 0 || std::memcmp (data.data() + 8, "WAVE", 4) != 0)
        throw std::runtime_error (path + ": not a RIFF/WAVE file");

    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t sampleRate = 0;
    const uint8_t* pcm = nullptr;
    size_t pcmBytes = 0;

    size_t pos = 12;
    while (pos + 8 <= data.size())
    {
        const uint8_t* chunk = data.data() + pos;
        const uint32_t size = readU32 (chunk + 4);
        const size_t body = pos + 8;
        const size_t available = std::min<size_t> (size, data.size() - body);

        if (std::memcmp (chunk, "fmt ", 4) == 0 && available >= 16)
        {
            format = readU16 (chunk + 8);
            channels = readU16 (chunk + 10);
            sampleRate = readU32 (chunk + 12);
            bits = readU16 (chunk + 22);
            if (format == kFormatExtensible && available >= 26)
                format = readU16 (chunk + 8 + 24); // first two bytes of SubFormat GUID
        }
        else if (std::memcmp (chunk, "data", 4) == 0)
        {
            pcm = data.data() + body;
            pcmBytes = available;
        }

        pos = body + size + (size & 1u);
    }

    if (pcm == nullptr || channels == 0 || sampleRate == 0)
        throw std::runtime_error (path + ": missing fmt or data chunk");

    const bool isFloat = format == kFormatFloat;
    if (! ((format == kFormatPcm && (bits == 16 || bits == 24 || bits == 32))
           || (isFloat && (bits == 32 || bits == 64))))
        throw std::runtime_error (path + ": unsupported WAV encoding");

    const size_t bytesPerSample = bits / 8u;
    const size_t frameBytes = bytesPerSample * channels;
    const size_t numFrames = pcmBytes / frameBytes;

    AudioBuffer out;
    out.sampleRate = sampleRate;
    out.resize (channels, static_cast<int> (numFrames));

    for (size_t i = 0; i < numFrames; ++i)
    {
        for (size_t c = 0; c < channels; ++c)
        {
            const uint8_t* s = pcm + i * frameBytes + c * bytesPerSample;
            float v = 0.0f;
            if (isFloat && bits == 32)
            {
                uint32_t u = readU32 (s);
                std::memcpy (&v, &u, 4);
            }
            else if (isFloat && bits == 64)
            {
                uint64_t u = static_cast<uint64_t> (readU32 (s)) | (static_cast<uint64_t> (readU32 (s + 4)) << 32);
                double d;
                std::memcpy (&d, &u, 8);
                v = static_cast<float> (d);
            }
            else if (bits == 16)
            {
                v = static_cast<float> (static_cast<int16_t> (readU16 (s))) / 32768.0f;
            }
            else if (bits == 24)
            {
                int32_t x = static_cast<int32_t> (s[0] | (s[1] << 8) | (s[2] << 16));
                if (x & 0x800000)
                    x |= ~0xFFFFFF;
                v = static_cast<float> (x) / 8388608.0f;
            }
            else // 32-bit int
            {
                v = static_cast<float> (static_cast<double> (static_cast<int32_t> (readU32 (s))) / 2147483648.0);
            }
            out.channels[c][i] = v;
        }
    }
    return out;
}

void writeWav (const std::string& path, const AudioBuffer& audio, WavFormat fmt)
{
    const int numCh = audio.numChannels();
    const int numSmp = audio.numSamples();
    if (numCh <= 0)
        throw std::runtime_error ("writeWav: no channels");

    const uint16_t bits = fmt == WavFormat::Float32 ? 32 : (fmt == WavFormat::Pcm24 ? 24 : 16);
    const uint16_t formatTag = fmt == WavFormat::Float32 ? kFormatFloat : kFormatPcm;
    const uint32_t bytesPerSample = bits / 8u;
    const uint32_t dataBytes = static_cast<uint32_t> (numSmp) * static_cast<uint32_t> (numCh) * bytesPerSample;
    const uint32_t sr = static_cast<uint32_t> (std::lround (audio.sampleRate));

    std::vector<uint8_t> v;
    v.reserve (44 + dataBytes);
    putTag (v, "RIFF");
    putU32 (v, 36 + dataBytes + (dataBytes & 1u));
    putTag (v, "WAVE");
    putTag (v, "fmt ");
    putU32 (v, 16);
    putU16 (v, formatTag);
    putU16 (v, static_cast<uint16_t> (numCh));
    putU32 (v, sr);
    putU32 (v, sr * static_cast<uint32_t> (numCh) * bytesPerSample);
    putU16 (v, static_cast<uint16_t> (static_cast<uint32_t> (numCh) * bytesPerSample));
    putU16 (v, bits);
    putTag (v, "data");
    putU32 (v, dataBytes);

    for (int i = 0; i < numSmp; ++i)
    {
        for (int c = 0; c < numCh; ++c)
        {
            const float x = audio.channels[static_cast<size_t> (c)][static_cast<size_t> (i)];
            if (fmt == WavFormat::Float32)
            {
                uint32_t u;
                std::memcpy (&u, &x, 4);
                putU32 (v, u);
            }
            else
            {
                const float clamped = std::clamp (x, -1.0f, 1.0f);
                if (fmt == WavFormat::Pcm24)
                {
                    const int32_t s = std::min<int32_t> (static_cast<int32_t> (std::lround (clamped * 8388608.0f)), 8388607);
                    v.push_back (static_cast<uint8_t> (s & 0xff));
                    v.push_back (static_cast<uint8_t> ((s >> 8) & 0xff));
                    v.push_back (static_cast<uint8_t> ((s >> 16) & 0xff));
                }
                else
                {
                    const int32_t s = std::min<int32_t> (static_cast<int32_t> (std::lround (clamped * 32768.0f)), 32767);
                    putU16 (v, static_cast<uint16_t> (static_cast<int16_t> (s)));
                }
            }
        }
    }
    if (dataBytes & 1u)
        v.push_back (0);

    std::ofstream out (path, std::ios::binary);
    if (! out)
        throw std::runtime_error ("cannot write " + path);
    out.write (reinterpret_cast<const char*> (v.data()), static_cast<std::streamsize> (v.size()));
    if (! out)
        throw std::runtime_error ("write failed: " + path);
}

} // namespace mse
