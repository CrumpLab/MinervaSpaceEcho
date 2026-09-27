#include "ParamPanel.h"

#include "Theme.h"
#include "mse/Params.h"

namespace {

struct PageSpec
{
    const char* name;
    std::vector<const char*> ids;
};

// Which parameters appear on which page (a parameter may appear on several).
const std::vector<PageSpec>& pageSpecs()
{
    static const std::vector<PageSpec> specs {
        { "Main", { "sync_mode", "trace_division", "trace_ms", "capacity", "power", "cue_mode", "mode_selector",
                    "blend_domain", "sequence_context", "address", "freeze", "feedback", "echo_level_db", "dry_level_db" } },
        { "Memory", { "capacity", "memory_budget", "full_policy", "merge_threshold", "freeze", "write_mode", "write_gate_db",
                      "novelty_mode", "novelty_threshold", "write_probability", "record_source", "clamp_incoming",
                      "clamp_budget", "clamp_protects" } },
        { "Forgetting", { "encoding_failure", "content_dropout", "decay_forget", "decay_fade_db", "wear_tone" } },
        { "Address", { "address", "spectrum_weight", "pitch_class_weight", "pitch_weight", "timbre_weight", "rhythm_weight",
                       "feature_focus", "feature_mode", "ternary_threshold", "similarity", "power" } },
        { "Retrieval", { "power", "similarity", "address", "feature_mode", "ternary_threshold", "self_match", "cue_gate_db",
                         "negative_mode", "normalization", "level_tracking", "recency", "feature_focus", "cue_noise",
                         "habituation" } },
        { "Cueing", { "cue_mode", "cue_source", "progressive_start", "rolling_window_ms", "rolling_interval_ms",
                      "lookahead_ms", "cue_smoothing_ms" } },
        { "Sequence", { "sequence_context", "context_cue", "context_weight", "cue_source", "chain_input", "chain_step",
                        "address", "cue_noise", "habituation", "power", "self_match", "playback", "cue_mode" } },
        { "Heads", { "mode_selector", "head1_level_db", "head1_pan", "head2_mode", "head2_level_db", "head2_pan", "head3_mode",
                     "head3_level_db", "head3_pan", "playback", "voices", "voice_spread", "voice_detune", "voice_delay_ms" } },
        { "Tape", { "length_mismatch", "wow", "flutter", "tape_drive", "hiss_db", "feedback_bass_db", "feedback_treble_db",
                    "spring_level_db", "spring_decay", "spring_on_dry", "echo_tone_hz", "intensity_to_tone",
                    "intensity_to_feedback" } },
        { "Spectral", { "blend_domain", "spectral_voices", "spectral_freeze", "max_active", "length_mismatch" } },
        { "Mix", { "feedback", "echo_level_db", "dry_level_db", "edge_fade_ms", "output_gain_db" } },
        { "Control", { "running", "midi_control", "midi_channel", "midi_base_note", "capture", "trigger_clamp_last", "trigger_clamp_all",
                       "trigger_unclamp_all", "trigger_clear_unclamped", "trigger_clear_all", "embed_memory" } },
    };
    return specs;
}

juce::String formatValue (const mse::ParamSpec& s, double v)
{
    const juce::String unit (s.unit);
    if (juce::String (s.id) == "midi_channel")
        return v < 0.5 ? juce::String ("Any") : juce::String (juce::roundToInt (v));
    if (juce::String (s.id) == "midi_base_note")
    {
        static const char* const names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        const int n = juce::roundToInt (v);
        return juce::String (names[n % 12]) + juce::String (n / 12 - 2) + " (" + juce::String (n) + ")";
    }
    if (unit == "dB" && v <= s.min + 1.0e-3 && (s.min <= mse::kLevelOffDb))
        return "off";
    if (s.type == mse::ParamType::Int)
        return juce::String (juce::roundToInt (v)) + (unit.isEmpty() ? "" : " " + unit);
    const double a = std::abs (v);
    const int decimals = a >= 100.0 ? 0 : (a >= 10.0 ? 1 : 2);
    if (unit == "ms" && a >= 1000.0)
        return juce::String (v / 1000.0, 2) + " s";
    if (unit == "Hz" && a >= 1000.0)
        return juce::String (v / 1000.0, 1) + " kHz";
    return juce::String (v, decimals) + (unit.isEmpty() ? "" : " " + unit);
}

constexpr int kCellW = 92, kWideW = 150, kCellH = 94;

} // namespace

// One parameter: its name above a knob, menu or switch.
class ParamPanel::Control final : public juce::Component
{
public:
    Control (juce::AudioProcessorValueTreeState& state, const mse::ParamSpec& s) : spec (s)
    {
        label.setText (s.name, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setFont (juce::FontOptions (12.0f));
        label.setColour (juce::Label::textColourId, theme::textDim);
        label.setMinimumHorizontalScale (0.7f);
        addAndMakeVisible (label);

        switch (s.type)
        {
            case mse::ParamType::Float:
            case mse::ParamType::Int:
                slider = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
                slider->setTextBoxStyle (juce::Slider::TextBoxBelow, false, kCellW - 4, 18);
                slider->setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
                slider->setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
                slider->setColour (juce::Slider::textBoxTextColourId, theme::text);
                addAndMakeVisible (*slider);
                sliderAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, s.id, *slider);
                slider->textFromValueFunction = [this] (double v) { return formatValue (spec, v); };
                slider->updateText();
                slider->setDoubleClickReturnValue (true, s.def);
                break;
            case mse::ParamType::Choice:
                combo = std::make_unique<juce::ComboBox>();
                for (int c = 0; c < s.numChoices; ++c)
                    combo->addItem (s.choices[c], c + 1);
                addAndMakeVisible (*combo);
                comboAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, s.id, *combo);
                break;
            case mse::ParamType::Bool:
                toggle = std::make_unique<juce::ToggleButton> ("");
                addAndMakeVisible (*toggle);
                buttonAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (state, s.id, *toggle);
                toggle->onStateChange = [this] { toggle->setButtonText (toggle->getToggleState() ? "On" : "Off"); };
                toggle->setButtonText (toggle->getToggleState() ? "On" : "Off");
                break;
        }
    }

    const char* id() const noexcept { return spec.id; }

    int preferredWidth() const { return spec.type == mse::ParamType::Float || spec.type == mse::ParamType::Int ? kCellW : kWideW; }

    void resized() override
    {
        auto r = getLocalBounds().reduced (3, 2);
        label.setBounds (r.removeFromTop (16));
        if (slider)
            slider->setBounds (r);
        if (combo)
            combo->setBounds (r.withSizeKeepingCentre (r.getWidth(), 26).translated (0, -8));
        if (toggle)
            toggle->setBounds (r.withSizeKeepingCentre (70, 26).translated (0, -8));
    }

private:
    const mse::ParamSpec& spec;
    juce::Label label;
    std::unique_ptr<juce::Slider> slider;
    std::unique_ptr<juce::ComboBox> combo;
    std::unique_ptr<juce::ToggleButton> toggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sliderAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> comboAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> buttonAttachment;
};

// A page: controls flowing left to right, then an optional note.
class ParamPanel::Page final : public juce::Component
{
public:
    Page (juce::AudioProcessorValueTreeState& state, const PageSpec& spec) : name (spec.name)
    {
        for (const char* id : spec.ids)
        {
            const int index = mse::findParam (id);
            if (index < 0)
                continue;
            auto* c = controls.add (new Control (state, mse::paramSpecs()[static_cast<size_t> (index)]));
            content.addAndMakeVisible (c);
        }
        note.setFont (juce::FontOptions (12.0f));
        note.setColour (juce::Label::textColourId, theme::textDim);
        note.setJustificationType (juce::Justification::topLeft);
        content.addAndMakeVisible (note);
        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (true, false);
        viewport.setScrollBarThickness (8);
        addAndMakeVisible (viewport);
    }

    void setNote (const juce::String& text)
    {
        note.setText (text, juce::dontSendNotification);
        resized();
    }

    void resized() override
    {
        viewport.setBounds (getLocalBounds());
        const int width = getWidth() - 12;
        int x = 6, y = 6;
        for (auto* c : controls)
        {
            const int w = c->preferredWidth();
            if (x + w > width && x > 6)
            {
                x = 6;
                y += kCellH;
            }
            c->setBounds (x, y, w, kCellH);
            x += w;
        }
        y += kCellH;
        const int noteH = note.getText().isEmpty() ? 0 : 20 + 15 * note.getText().length() / juce::jmax (1, width / 7);
        note.setBounds (8, y, width - 4, noteH);
        content.setSize (getWidth() - (y + noteH > getHeight() ? 8 : 0), juce::jmax (getHeight(), y + noteH + 6));
    }

    const juce::String name;
    juce::OwnedArray<Control>& getControls() noexcept { return controls; }

private:
    juce::Viewport viewport;
    juce::Component content;
    juce::OwnedArray<Control> controls;
    juce::Label note;
};

ParamPanel::ParamPanel (juce::AudioProcessorValueTreeState& s) : state (s)
{
    for (const auto& spec : pageSpecs())
    {
        auto* page = pages.add (new Page (state, spec));
        tabs.addTab (spec.name, theme::panel, page, false);
    }
    tabs.setTabBarDepth (28);
    tabs.setOutline (0);
    addAndMakeVisible (tabs);
}

ParamPanel::~ParamPanel() = default;

void ParamPanel::resized()
{
    tabs.setBounds (getLocalBounds());
}

void ParamPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);
}

int ParamPanel::getCurrentPage() const
{
    return tabs.getCurrentTabIndex();
}

void ParamPanel::setCurrentPage (int index)
{
    if (index >= 0 && index < tabs.getNumTabs())
        tabs.setCurrentTabIndex (index);
}

void ParamPanel::setPageNote (const juce::String& page, const juce::String& text)
{
    for (auto* p : pages)
        if (p->name == page)
            p->setNote (text);
}

void ParamPanel::updateRelevance()
{
    const auto value = [this] (const char* id) { return juce::roundToInt (state.getRawParameterValue (id)->load()); };
    const int sync = value ("sync_mode"), cue = value ("cue_mode"), playback = value ("playback");
    const int selector = value ("mode_selector"), domain = value ("blend_domain"), mismatch = value ("length_mismatch");
    const bool spectral = domain == static_cast<int> (mse::BlendDomain::Spectral) || mismatch == static_cast<int> (mse::LengthMismatch::Stretch);
    const bool custom = selector == static_cast<int> (mse::ModeSelector::Custom);

    for (auto* page : pages)
        for (auto* c : page->getControls())
        {
            const juce::String id (c->id());
            bool relevant = true;
            if (id == "trace_ms")
                relevant = sync == static_cast<int> (mse::SyncMode::Free);
            else if (id == "trace_division")
                relevant = sync == static_cast<int> (mse::SyncMode::Tempo);
            else if (id == "progressive_start")
                relevant = cue == static_cast<int> (mse::CueMode::Progressive);
            else if (id.startsWith ("rolling_"))
                relevant = cue == static_cast<int> (mse::CueMode::Rolling);
            else if (id == "voices" || id.startsWith ("voice_"))
                relevant = playback == static_cast<int> (mse::Playback::Voices);
            else if (id == "spectral_voices" || id == "spectral_freeze")
                relevant = spectral;
            else if (id == "ternary_threshold")
                relevant = value ("feature_mode") == static_cast<int> (mse::FeatureMode::Ternary);
            else if (id == "novelty_threshold")
                relevant = value ("novelty_mode") != static_cast<int> (mse::NoveltyMode::Off);
            else if (id == "head2_mode" || id == "head3_mode")
                relevant = custom;
            else if (id == "context_cue")
                relevant = value ("sequence_context") != 0;
            else if (id == "context_weight")
                relevant = value ("sequence_context") != 0 && value ("context_cue") == static_cast<int> (mse::ContextCue::MatchBoth)
                           && value ("cue_source") != static_cast<int> (mse::CueSource::EchoChain);
            else if (id == "chain_input" || id == "chain_step")
                relevant = value ("cue_source") == static_cast<int> (mse::CueSource::EchoChain);
            else if (id.endsWith ("_weight") && id != "context_weight")
                relevant = value ("address") == static_cast<int> (mse::AddressMode::Custom);
            else if (id == "feature_focus")
                // Focus applies to the Spectrum set.
                relevant = value ("address") == static_cast<int> (mse::AddressMode::Spectrum)
                           || (value ("address") == static_cast<int> (mse::AddressMode::Custom)
                               && state.getRawParameterValue ("spectrum_weight")->load() > 0.0f);
            else if (id == "midi_channel" || id == "midi_base_note")
                relevant = value ("midi_control") != 0;
            c->setAlpha (relevant ? 1.0f : 0.4f);
        }
}
