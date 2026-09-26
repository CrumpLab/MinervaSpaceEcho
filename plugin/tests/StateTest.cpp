// Headless check of plugin state: parameters and (optionally embedded) memory
// survive getStateInformation -> setStateInformation into a fresh instance.
#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>
#include <cstdio>

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

namespace {

void setParam (juce::AudioProcessor& p, const juce::String& id, float realValue)
{
    for (auto* param : p.getParameters())
        if (auto* withId = dynamic_cast<juce::RangedAudioParameter*> (param); withId && withId->getParameterID() == id)
        {
            withId->setValueNotifyingHost (withId->convertTo0to1 (realValue));
            return;
        }
    std::printf ("no parameter %s\n", id.toRawUTF8());
    std::exit (1);
}

float getParam (juce::AudioProcessor& p, const juce::String& id)
{
    for (auto* param : p.getParameters())
        if (auto* withId = dynamic_cast<juce::RangedAudioParameter*> (param); withId && withId->getParameterID() == id)
            return withId->convertFrom0to1 (withId->getValue());
    return NAN;
}

// Plays `seconds` of a chord through the processor; returns the output RMS
// (ignoring the first 50 ms, where parameter changes are still ramping).
double play (juce::AudioProcessor& p, double seconds, float amplitude = 0.3f)
{
    const int block = 512;
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    double phase = 0.0, sum = 0.0;
    long n = 0;
    const long total = static_cast<long> (seconds * 48000.0);
    for (long done = 0; done < total; done += block)
    {
        for (int i = 0; i < block; ++i)
        {
            const float x = amplitude * static_cast<float> (std::sin (phase) + 0.5 * std::sin (1.5 * phase));
            phase += 2.0 * 3.14159265 * 220.0 / 48000.0;
            buf.setSample (0, i, x);
            buf.setSample (1, i, x);
        }
        p.processBlock (buf, midi);
        if (done < 2400)
            continue;
        for (int i = 0; i < block; ++i)
        {
            sum += buf.getSample (0, i) * buf.getSample (0, i);
            ++n;
        }
    }
    return std::sqrt (sum / std::max (1L, n));
}

int check (bool ok, const char* what)
{
    std::printf ("%s %s\n", ok ? "PASS" : "FAIL", what);
    return ok ? 0 : 1;
}

int run (bool embed)
{
    std::unique_ptr<juce::AudioProcessor> a (createPluginFilter());
    a->setPlayConfigDetails (2, 2, 48000.0, 512);
    a->prepareToPlay (48000.0, 512);
    setParam (*a, "sync_mode", 0.0f);   // Free
    setParam (*a, "trace_ms", 500.0f);
    setParam (*a, "power", 7.0f);
    setParam (*a, "embed_memory", embed ? 1.0f : 0.0f);
    play (*a, 3.0); // six 500 ms traces

    juce::MemoryBlock state;
    a->getStateInformation (state);

    std::unique_ptr<juce::AudioProcessor> b (createPluginFilter());
    b->setPlayConfigDetails (2, 2, 48000.0, 512);
    b->setStateInformation (state.getData(), static_cast<int> (state.getSize()));
    b->prepareToPlay (48000.0, 512);

    int failures = 0;
    failures += check (std::abs (getParam (*b, "power") - 7.0f) < 1e-3f, "power restored");
    failures += check (std::abs (getParam (*b, "trace_ms") - 500.0f) < 1.0f, "trace length restored");

    // With memory restored, a frozen instance still echoes; without, it is silent.
    setParam (*b, "freeze", 1.0f);
    setParam (*b, "dry_level_db", -60.0f);
    const double echo = play (*b, 1.0);
    if (embed)
        failures += check (echo > 0.05, "embedded memory restored (echo present)");
    else
        failures += check (echo < 1e-6, "memory not embedded by default (no echo)");
    std::printf ("  state %zu bytes, echo rms %.4f\n", static_cast<size_t> (state.getSize()), echo);
    return failures;
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    int failures = run (false) + run (true);
    std::printf (failures == 0 ? "ALL PASSED\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
