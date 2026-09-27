#include "PluginProcessor.h"
#include "PluginEditor.h"

#include "ExamplePresets.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include "FactoryPresets.h"
#include "mse/Import.h"
#include "mse/Preset.h"

namespace {

// Plugin state layout when memory is embedded (otherwise JUCE's plain XML
// binary is used, as in earlier versions):
//   "MSE3" | u32 xml length | xml | u64 memory length | memory blob
constexpr char kStateMagic[4] = { 'M', 'S', 'E', '3' };

juce::NormalisableRange<float> rangeFor (const mse::ParamSpec& s)
{
    juce::NormalisableRange<float> r (s.min, s.max, s.type == mse::ParamType::Int ? 1.0f : 0.0f);
    if (s.skewCentre > s.min && s.skewCentre < s.max)
        r.setSkewForCentre (s.skewCentre);
    return r;
}

} // namespace

MinervaSpaceEchoProcessor::MinervaSpaceEchoProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                          .withInput ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      parameters (*this, nullptr, "MinervaSpaceEcho", createLayout())
{
    const auto& specs = mse::paramSpecs();
    for (size_t i = 0; i < specs.size(); ++i)
        rawParams[i] = parameters.getRawParameterValue (specs[i].id);

    engine.enableMemoryView();
    startTimerHz (20);
}

MinervaSpaceEchoProcessor::~MinervaSpaceEchoProcessor()
{
    aliveFlag->store (false);
    stopTimer();
}

juce::AudioProcessorValueTreeState::ParameterLayout MinervaSpaceEchoProcessor::createLayout()
{
    // Built from the engine's parameter table: IDs match mse-render presets.
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    for (const auto& s : mse::paramSpecs())
    {
        const juce::ParameterID id { s.id, 1 };
        switch (s.type)
        {
            case mse::ParamType::Float:
                layout.add (std::make_unique<juce::AudioParameterFloat> (
                    id, s.name, rangeFor (s), s.def,
                    juce::AudioParameterFloatAttributes().withLabel (s.unit).withAutomatable (s.automatable)));
                break;
            case mse::ParamType::Int:
                layout.add (std::make_unique<juce::AudioParameterInt> (
                    id, s.name, static_cast<int> (s.min), static_cast<int> (s.max), static_cast<int> (s.def),
                    juce::AudioParameterIntAttributes().withLabel (s.unit).withAutomatable (s.automatable)));
                break;
            case mse::ParamType::Bool:
                layout.add (std::make_unique<juce::AudioParameterBool> (
                    id, s.name, s.def > 0.5f, juce::AudioParameterBoolAttributes().withAutomatable (s.automatable)));
                break;
            case mse::ParamType::Choice:
            {
                juce::StringArray choices;
                for (int c = 0; c < s.numChoices; ++c)
                    choices.add (s.choices[c]);
                layout.add (std::make_unique<juce::AudioParameterChoice> (
                    id, s.name, choices, static_cast<int> (s.def),
                    juce::AudioParameterChoiceAttributes().withAutomatable (s.automatable)));
                break;
            }
        }
    }
    return layout;
}

mse::EngineParams MinervaSpaceEchoProcessor::readParams() const noexcept
{
    mse::ParamValues v {};
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = rawParams[i]->load (std::memory_order_relaxed);
    return mse::paramsFromValues (v);
}

bool MinervaSpaceEchoProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    if (layouts.getMainInputChannelSet() != out)
        return false;
    // Optional sidechain (cues memory when Cue Source = Sidechain).
    if (layouts.inputBuses.size() > 1)
    {
        const auto sc = layouts.getChannelSet (true, 1);
        if (! sc.isDisabled() && sc != juce::AudioChannelSet::mono() && sc != juce::AudioChannelSet::stereo())
            return false;
    }
    return true;
}

void MinervaSpaceEchoProcessor::applyMemoryConfig()
{
    const auto p = readParams();
    mse::MemoryConfig cfg = engine.getMemoryConfig();
    cfg.capacity = p.capacity;
    cfg.budgetBytes = mse::memoryBudgetBytes (p.memoryBudgetIndex);
    engine.setMemoryConfig (cfg); // no-op when unchanged
}

void MinervaSpaceEchoProcessor::saveMemory (const juce::File& folder, std::function<void (juce::String)> done)
{
    auto snap = std::make_shared<mse::MemorySnapshot>();
    if (! engine.takeSnapshot (*snap))
    {
        done ("Memory was busy; try again.");
        return;
    }
    const auto& specs = mse::paramSpecs();
    for (size_t i = 0; i < specs.size(); ++i)
        snap->params.emplace_back (specs[i].id, rawParams[i]->load());

    const auto path = folder.getFullPathName().toStdString();
    juce::Thread::launch ([snap, path, done = std::move (done)] {
        juce::String error;
        try
        {
            mse::writeMemoryFolder (*snap, path);
        }
        catch (const std::exception& e)
        {
            error = e.what();
        }
        juce::MessageManager::callAsync ([done, error] { done (error); });
    });
}

juce::String MinervaSpaceEchoProcessor::loadMemory (const juce::File& target)
{
    const auto folder = target.isDirectory() ? target : target.getParentDirectory();
    try
    {
        engine.loadSnapshot (mse::readMemoryFolder (folder.getFullPathName().toStdString()));
        return {};
    }
    catch (const std::exception& e)
    {
        return e.what();
    }
}

void MinervaSpaceEchoProcessor::timerCallback()
{
    // Message thread: structural changes (memory reallocation) and freeing
    // memory the audio thread has let go of.
    applyMemoryConfig();
    engine.collectGarbage();

    // A Mode Selector change from MIDI: make the parameter follow.
    if (const int mode = pendingModeParam.exchange (-1); mode >= 0)
        setParamValue (mse::kModeSelector, static_cast<float> (mode));
}

void MinervaSpaceEchoProcessor::setParamValue (int index, float realValue)
{
    if (auto* param = dynamic_cast<juce::RangedAudioParameter*> (
            parameters.getParameter (mse::paramSpecs()[static_cast<size_t> (index)].id)))
    {
        param->beginChangeGesture();
        param->setValueNotifyingHost (param->convertTo0to1 (realValue));
        param->endChangeGesture();
    }
}

void MinervaSpaceEchoProcessor::handleMidi (const juce::MidiBuffer& midi, const mse::EngineParams& p) noexcept
{
    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();
        if (p.midiChannel > 0 && m.getChannel() != p.midiChannel)
            continue;
        if (m.isAllNotesOff() || m.isAllSoundOff())
        {
            midiFreeze = midiSpectralFreeze = midiChain = false;
            continue;
        }
        const bool on = m.isNoteOn();
        if (! on && ! m.isNoteOff())
            continue;
        const auto mapping = mse::midiMappingFor (m.getNoteNumber(), p.midiBaseNote);
        if (on)
        {
            lastMidiNote.store (m.getNoteNumber(), std::memory_order_relaxed);
            midiCount.fetch_add (1, std::memory_order_relaxed);
        }
        switch (mapping.action)
        {
            case mse::MidiAction::Freeze:         midiFreeze = on; break;
            case mse::MidiAction::SpectralFreeze: midiSpectralFreeze = on; break;
            case mse::MidiAction::EchoChain:      midiChain = on; break;
            case mse::MidiAction::None:           break;
            case mse::MidiAction::ModeSelector:
                if (on)
                {
                    modeOverride = mapping.value;
                    modeOverrideSamples = 0;
                    pendingModeParam.store (mapping.value);
                }
                break;
            case mse::MidiAction::Capture:        if (on) engine.sendCommandFromAudioThread (mse::Command::Capture); break;
            case mse::MidiAction::ClampLast:      if (on) engine.sendCommandFromAudioThread (mse::Command::ClampLast); break;
            case mse::MidiAction::ClampAll:       if (on) engine.sendCommandFromAudioThread (mse::Command::ClampAll); break;
            case mse::MidiAction::UnclampAll:     if (on) engine.sendCommandFromAudioThread (mse::Command::UnclampAll); break;
            case mse::MidiAction::ClearUnclamped: if (on) engine.sendCommandFromAudioThread (mse::Command::ClearUnclamped); break;
            case mse::MidiAction::ClearAll:       if (on) engine.sendCommandFromAudioThread (mse::Command::ClearAll); break;
        }
    }
}

void MinervaSpaceEchoProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    applyMemoryConfig();
    engine.setParams (readParams());
    engine.prepare (sampleRate, samplesPerBlock, getTotalNumOutputChannels());
    fallbackPpq = 0.0;
}

double MinervaSpaceEchoProcessor::getTailLengthSeconds() const
{
    const auto p = readParams();
    const double bpm = std::max (20.0, uiClock.bpm.load());
    const double trace = p.syncMode == mse::SyncMode::Tempo
                             ? mse::divisionQuarters (p.traceDivision, 4, 4) * 60.0 / bpm
                             : p.traceMs * 0.001;
    // One repeat without feedback; a generous allowance with it; plus the spring's tail.
    const double spring = p.springLevelDb > mse::kLevelOffDb ? p.springDecaySeconds : 0.0;
    return std::min (60.0, trace * (p.feedback > 0.0f ? 8.0 : 1.0) + spring);
}

mse::HostClock MinervaSpaceEchoProcessor::readHostClock()
{
    mse::HostClock clock;
    if (auto* ph = getPlayHead())
    {
        if (const auto pos = ph->getPosition())
        {
            if (const auto bpm = pos->getBpm())
            {
                clock.hasTempo = true;
                clock.bpm = *bpm;
            }
            if (const auto ppq = pos->getPpqPosition())
                clock.ppqPosition = *ppq;
            if (const auto sig = pos->getTimeSignature())
            {
                clock.timeSigNumerator = sig->numerator;
                clock.timeSigDenominator = sig->denominator;
            }
            clock.isPlaying = pos->getIsPlaying();
        }
    }
    if (! clock.isPlaying)
        clock.ppqPosition = fallbackPpq;
    return clock;
}

void MinervaSpaceEchoProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    for (auto ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    const auto clock = readHostClock();
    // Keep a free-running position so the engine always has a clock, even with
    // the transport stopped (plan §6).
    fallbackPpq = clock.ppqPosition + buffer.getNumSamples() / getSampleRate() * clock.bpm / 60.0;

    auto params = readParams();
    if (params.midiControl)
        handleMidi (midi, params);
    else
        midiFreeze = midiSpectralFreeze = midiChain = false;
    params.freeze = params.freeze || midiFreeze;
    if (midiChain)
        params.cueSource = mse::CueSource::EchoChain;
    params.spectralFreeze = params.spectralFreeze || midiSpectralFreeze;
    if (modeOverride >= 0)
    {
        // Until the parameter has caught up (or half a second has passed).
        modeOverrideSamples += buffer.getNumSamples();
        if (static_cast<int> (params.modeSelector) == modeOverride || modeOverrideSamples > getSampleRate() * 0.5)
            modeOverride = -1;
        else
            params.modeSelector = static_cast<mse::ModeSelector> (modeOverride);
    }
    engine.setParams (params);
    auto main = getBusBuffer (buffer, false, 0);
    const float* const* sidechain = nullptr;
    int sidechainChannels = 0;
    if (getBusCount (true) > 1 && getBus (true, 1)->isEnabled())
    {
        auto sc = getBusBuffer (buffer, true, 1);
        sidechainChannels = sc.getNumChannels();
        sidechain = sidechainChannels > 0 ? sc.getArrayOfReadPointers() : nullptr;
    }
    engine.process (main.getArrayOfWritePointers(), main.getNumChannels(), main.getNumSamples(), clock, sidechain,
                    sidechainChannels);

    uiClock.hasTempo.store (clock.hasTempo, std::memory_order_relaxed);
    uiClock.isPlaying.store (clock.isPlaying, std::memory_order_relaxed);
    uiClock.bpm.store (clock.bpm, std::memory_order_relaxed);
    uiClock.ppq.store (clock.ppqPosition, std::memory_order_relaxed);
    uiClock.sigNum.store (clock.timeSigNumerator, std::memory_order_relaxed);
    uiClock.sigDen.store (clock.timeSigDenominator, std::memory_order_relaxed);
}

mse::HostClock MinervaSpaceEchoProcessor::getClockForUi() const noexcept
{
    mse::HostClock c;
    c.hasTempo = uiClock.hasTempo.load (std::memory_order_relaxed);
    c.isPlaying = uiClock.isPlaying.load (std::memory_order_relaxed);
    c.bpm = uiClock.bpm.load (std::memory_order_relaxed);
    c.ppqPosition = uiClock.ppq.load (std::memory_order_relaxed);
    c.timeSigNumerator = uiClock.sigNum.load (std::memory_order_relaxed);
    c.timeSigDenominator = uiClock.sigDen.load (std::memory_order_relaxed);
    return c;
}

juce::AudioProcessorEditor* MinervaSpaceEchoProcessor::createEditor()
{
    return new MinervaSpaceEchoEditor (*this);
}

void MinervaSpaceEchoProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    state.setProperty ("stateVersion", kStateVersion, nullptr);
    auto xml = state.createXml();
    if (xml == nullptr)
        return;

    mse::MemorySnapshot snap;
    if (! readParams().embedMemory || ! engine.takeSnapshot (snap))
    {
        copyXmlToBinary (*xml, destData);
        return;
    }

    // Memory embedded in the Live set (off by default: sets can get large).
    const auto xmlText = xml->toString().toStdString();
    const auto blob = mse::serializeMemory (snap);
    juce::MemoryOutputStream out (destData, false);
    out.write (kStateMagic, 4);
    out.writeInt (static_cast<int> (xmlText.size()));
    out.write (xmlText.data(), xmlText.size());
    out.writeInt64 (static_cast<juce::int64> (blob.size()));
    out.write (blob.data(), blob.size());
}

void MinervaSpaceEchoProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (sizeInBytes > 8 && std::memcmp (data, kStateMagic, 4) == 0)
    {
        juce::MemoryInputStream in (data, static_cast<size_t> (sizeInBytes), false);
        in.skipNextBytes (4);
        const int xmlLen = in.readInt();
        if (xmlLen <= 0 || xmlLen > in.getNumBytesRemaining())
            return;
        juce::MemoryBlock xmlBytes;
        in.readIntoMemoryBlock (xmlBytes, xmlLen);
        if (auto xml = juce::parseXML (xmlBytes.toString()))
            if (xml->hasTagName (parameters.state.getType()))
                parameters.replaceState (juce::ValueTree::fromXml (*xml));

        const auto blobLen = in.readInt64();
        if (blobLen > 0 && blobLen <= in.getNumBytesRemaining())
        {
            juce::MemoryBlock blob;
            in.readIntoMemoryBlock (blob, static_cast<ssize_t> (blobLen));
            try
            {
                applyMemoryConfig(); // fit the restored capacity before loading
                engine.loadSnapshot (mse::deserializeMemory (static_cast<const uint8_t*> (blob.getData()), blob.getSize()));
            }
            catch (const std::exception&)
            {
                // A damaged memory blob must not prevent the parameters from loading.
            }
        }
        return;
    }

    // Plain parameter state. Parameters missing from older states keep their defaults.
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (parameters.state.getType()))
            parameters.replaceState (juce::ValueTree::fromXml (*xml));
}

// ---- importing audio ------------------------------------------------------------

bool MinervaSpaceEchoProcessor::isImportableFile (const juce::String& path)
{
    return juce::File (path).hasFileExtension ("wav;aif;aiff;flac;ogg;mp3;m4a;caf");
}

bool MinervaSpaceEchoProcessor::getLockImports() const
{
    return static_cast<bool> (parameters.state.getProperty ("lockImports", false));
}

void MinervaSpaceEchoProcessor::setLockImports (bool lock)
{
    parameters.state.setProperty ("lockImports", lock, nullptr);
}

mse::MemorySnapshot MinervaSpaceEchoProcessor::decodeForImport (const juce::StringArray& paths, const mse::ImportSettings& settings,
                                                                double targetRate, juce::StringArray& problems, int& files)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    mse::MemorySnapshot all;
    all.sampleRate = targetRate;
    files = 0;
    const double maxSeconds = settings.traceSeconds * settings.maxTraces + 1.0;
    for (const auto& path : paths)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (juce::File (path)));
        if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        {
            problems.add (juce::File (path).getFileName());
            continue;
        }
        const auto length = static_cast<int> (std::min<juce::int64> (reader->lengthInSamples,
                                                                    static_cast<juce::int64> (maxSeconds * reader->sampleRate)));
        const int channels = std::clamp (static_cast<int> (reader->numChannels), 1, 2);
        juce::AudioBuffer<float> buffer (channels, length);
        reader->read (&buffer, 0, length, 0, true, channels > 1);
        std::vector<std::vector<float>> audio (static_cast<size_t> (channels));
        for (int c = 0; c < channels; ++c)
            audio[static_cast<size_t> (c)].assign (buffer.getReadPointer (c), buffer.getReadPointer (c) + length);
        auto s = settings;
        s.maxTraces = settings.maxTraces - static_cast<int> (all.traces.size());
        if (s.maxTraces <= 0)
            break;
        auto snap = mse::tracesFromAudio (audio, reader->sampleRate, targetRate, s);
        for (auto& t : snap.traces)
            all.traces.push_back (std::move (t));
        ++files;
    }
    for (size_t i = 0; i < all.traces.size(); ++i)
        all.traces[i].serial = i + 1;
    return all;
}

void MinervaSpaceEchoProcessor::importAudioFiles (const juce::StringArray& paths, std::function<void (juce::String)> done)
{
    const auto p = readParams();
    mse::ImportSettings settings;
    settings.traceSeconds = engine.getStats().traceSeconds;
    if (settings.traceSeconds <= 0.0)
        settings.traceSeconds = p.syncMode == mse::SyncMode::Tempo
                                    ? mse::divisionQuarters (p.traceDivision, 4, 4) * 60.0 / std::max (20.0, uiClock.bpm.load())
                                    : p.traceMs * 0.001;
    settings.features = { p.featureMode, p.ternaryThreshold };
    settings.clamp = getLockImports();
    settings.maxTraces = p.capacity;
    const double targetRate = getSampleRate() > 0.0 ? getSampleRate() : 48000.0;
    auto alive = aliveFlag;

    juce::Thread::launch ([this, alive, paths, settings, targetRate, done = std::move (done)] {
        juce::StringArray problems;
        int files = 0;
        auto all = decodeForImport (paths, settings, targetRate, problems, files);
        juce::MessageManager::callAsync ([this, alive, all = std::move (all), problems, files, settings, done] {
            if (! alive->load())
                return;
            engine.importTraces (all);
            juce::String msg = "Added " + juce::String (static_cast<int> (all.traces.size())) + " traces ("
                               + juce::String (settings.traceSeconds, 2) + " s each) from " + juce::String (files)
                               + (files == 1 ? " file" : " files") + (settings.clamp ? ", clamped" : "");
            if (! problems.isEmpty())
                msg << ". Could not read: " << problems.joinIntoString (", ");
            done (msg);
        });
    });
}

// ---- presets ----------------------------------------------------------------------

juce::File MinervaSpaceEchoProcessor::userPresetFolder()
{
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory)
        .getChildFile ("MINERVA Space Echo")
        .getChildFile ("Presets");
}

namespace {

void addBuiltIn (juce::Array<MinervaSpaceEchoProcessor::Preset>& out, const juce::String& category, int count,
                 const char* const* names, const char* (*original) (const char*),
                 const char* (*get) (const char*, int&))
{
    juce::Array<MinervaSpaceEchoProcessor::Preset> found;
    for (int i = 0; i < count; ++i)
    {
        int size = 0;
        const char* data = get (names[i], size);
        const juce::String file = original (names[i]);
        if (data == nullptr || ! file.endsWithIgnoreCase (".txt"))
            continue;
        MinervaSpaceEchoProcessor::Preset p;
        p.name = file.dropLastCharacters (4).replaceCharacter ('_', ' ');
        p.category = category;
        p.text = juce::String::fromUTF8 (data, size);
        found.add (p);
    }
    std::sort (found.begin(), found.end(), [] (const auto& a, const auto& b) { return a.name < b.name; });
    out.addArray (found);
}

} // namespace

const juce::Array<MinervaSpaceEchoProcessor::Preset>& MinervaSpaceEchoProcessor::getPresets()
{
    if (! presetsScanned)
        rescanUserPresets();
    return presets;
}

void MinervaSpaceEchoProcessor::rescanUserPresets()
{
    presets.clear();
    addBuiltIn (presets, "Factory", FactoryPresets::namedResourceListSize, FactoryPresets::namedResourceList,
                            FactoryPresets::getNamedResourceOriginalFilename, FactoryPresets::getNamedResource);
    addBuiltIn (presets, "Examples", ExamplePresets::namedResourceListSize, ExamplePresets::namedResourceList,
                            ExamplePresets::getNamedResourceOriginalFilename, ExamplePresets::getNamedResource);
    auto files = userPresetFolder().findChildFiles (juce::File::findFiles, false, "*.txt");
    files.sort();
    for (const auto& f : files)
    {
        Preset p;
        p.name = f.getFileNameWithoutExtension();
        p.category = "User";
        p.file = f;
        presets.add (p);
    }
    presetsScanned = true;
}

juce::String MinervaSpaceEchoProcessor::applyPreset (const Preset& preset)
{
    const auto text = preset.file != juce::File() ? preset.file.loadFileAsString() : preset.text;
    // Everything a preset doesn't mention returns to its default; session
    // settings (memory budget, embedding, MIDI) are left alone.
    auto values = mse::defaultParamValues();
    for (int i = 0; i < mse::kNumParams; ++i)
        if (mse::isSessionParam (i))
            values[static_cast<size_t> (i)] = rawParams[static_cast<size_t> (i)]->load();
    try
    {
        std::vector<mse::TimedAssignment> timed; // render-only changes over time: ignored here
        mse::applyPresetText (values, text.toStdString(), &timed, preset.name.toStdString());
    }
    catch (const std::exception& e)
    {
        return e.what();
    }
    for (int i = 0; i < mse::kNumParams; ++i)
        if (! mse::isSessionParam (i) && ! juce::exactlyEqual (values[static_cast<size_t> (i)], rawParams[static_cast<size_t> (i)]->load()))
            setParamValue (i, values[static_cast<size_t> (i)]);
    parameters.state.setProperty ("presetName", preset.name, nullptr);
    return {};
}

juce::String MinervaSpaceEchoProcessor::saveUserPreset (const juce::String& name, const juce::String& description)
{
    const auto clean = juce::File::createLegalFileName (name.trim());
    if (clean.isEmpty())
        return "Please give the preset a name.";
    mse::ParamValues values {};
    for (size_t i = 0; i < values.size(); ++i)
        values[i] = rawParams[i]->load();
    const auto dir = userPresetFolder();
    dir.createDirectory();
    const auto file = dir.getChildFile (clean + ".txt");
    if (! file.replaceWithText (mse::writePreset (values, description.trim().toStdString())))
        return "Could not write " + file.getFullPathName();
    parameters.state.setProperty ("presetName", clean, nullptr);
    rescanUserPresets();
    return {};
}

juce::String MinervaSpaceEchoProcessor::getCurrentPresetName() const
{
    return parameters.state.getProperty ("presetName", "").toString();
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MinervaSpaceEchoProcessor();
}
