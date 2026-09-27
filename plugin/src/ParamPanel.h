#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// Tabbed pages of parameter controls, generated from the engine's parameter
// table: knobs for numbers, menus for choices, switches for on/off.
class ParamPanel final : public juce::Component
{
public:
    explicit ParamPanel (juce::AudioProcessorValueTreeState& state);
    ~ParamPanel() override;

    void resized() override;
    void paint (juce::Graphics&) override;

    int getCurrentPage() const;
    void setCurrentPage (int);

    // Dims controls that have no effect with the current settings.
    void updateRelevance();

    // A page's extra text (shown under its controls), e.g. the MIDI note map.
    void setPageNote (const juce::String& page, const juce::String& text);

private:
    juce::AudioProcessorValueTreeState& state;
    class Control;
    class Page;
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    juce::OwnedArray<Page> pages;
};
