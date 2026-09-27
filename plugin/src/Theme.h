#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Colours and look of the custom editor (plan Stage 7).
namespace theme {

const juce::Colour background { 0xff15171a };
const juce::Colour panel { 0xff1e2126 };
const juce::Colour panelLight { 0xff2a2e35 };
const juce::Colour outline { 0xff363b44 };
const juce::Colour text { 0xffe8e2d4 };
const juce::Colour textDim { 0xff9a968c };
const juce::Colour accent { 0xfff2a33a };     // amber: head 1, positive activation, locks
const juce::Colour head2 { 0xff4fc1b0 };      // teal
const juce::Colour head3 { 0xffb68cf0 };      // violet
const juce::Colour negative { 0xff4a90d9 };   // blue: negative values
const juce::Colour danger { 0xffe0604f };
const juce::Colour probe { 0xffe87ba4 };      // magenta: the probe (Stage 11)

juce::Colour headColour (int head);

// Diverging map for feature values (z-scores): blue < 0 < amber.
juce::Colour heat (float v) noexcept;
// Same for quantised thumbnails (value * 40); a lookup table.
juce::PixelARGB heatQ (int8_t q) noexcept;

class LookAndFeel final : public juce::LookAndFeel_V4
{
public:
    LookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float start, float end,
                           juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
    void drawTabButton (juce::TabBarButton&, juce::Graphics&, bool isMouseOver, bool isMouseDown) override;
    int getTabButtonBestWidth (juce::TabBarButton&, int tabDepth) override;
    void drawTabAreaBehindFrontButton (juce::TabbedButtonBar&, juce::Graphics&, int, int) override {}
    juce::Font getComboBoxFont (juce::ComboBox&) override { return juce::FontOptions (13.0f); }
    juce::Font getTextButtonFont (juce::TextButton&, int) override { return juce::FontOptions (13.0f); }
    juce::Font getLabelFont (juce::Label& l) override { return l.getFont(); }
};

} // namespace theme
