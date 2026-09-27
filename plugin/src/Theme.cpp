#include "Theme.h"

#include <array>

namespace theme {

juce::Colour headColour (int head)
{
    return head == 0 ? accent : (head == 1 ? head2 : head3);
}

juce::Colour heat (float v) noexcept
{
    const float t = juce::jlimit (-1.0f, 1.0f, v / 2.5f);
    const auto base = juce::Colour (0xff1a1d22);
    if (t >= 0.0f)
        return t < 0.6f ? base.interpolatedWith (accent, t / 0.6f)
                        : accent.interpolatedWith (juce::Colour (0xfffff0c8), (t - 0.6f) / 0.4f);
    const float u = -t;
    return u < 0.6f ? base.interpolatedWith (negative, u / 0.6f)
                    : negative.interpolatedWith (juce::Colour (0xffc8e4ff), (u - 0.6f) / 0.4f);
}

juce::PixelARGB heatQ (int8_t q) noexcept
{
    static const auto table = [] {
        std::array<juce::PixelARGB, 256> t {};
        for (int i = 0; i < 256; ++i)
            t[static_cast<size_t> (i)] = heat (static_cast<float> (static_cast<int8_t> (i)) / 40.0f).getPixelARGB();
        return t;
    }();
    return table[static_cast<uint8_t> (q)];
}

LookAndFeel::LookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, background);
    setColour (juce::Label::textColourId, text);
    setColour (juce::Slider::textBoxTextColourId, text);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::rotarySliderFillColourId, accent);
    setColour (juce::Slider::rotarySliderOutlineColourId, panelLight);
    setColour (juce::ComboBox::backgroundColourId, panelLight);
    setColour (juce::ComboBox::outlineColourId, outline);
    setColour (juce::ComboBox::textColourId, text);
    setColour (juce::ComboBox::arrowColourId, textDim);
    setColour (juce::PopupMenu::backgroundColourId, panel);
    setColour (juce::PopupMenu::textColourId, text);
    setColour (juce::PopupMenu::headerTextColourId, accent);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha (0.25f));
    setColour (juce::PopupMenu::highlightedTextColourId, text);
    setColour (juce::TextButton::buttonColourId, panelLight);
    setColour (juce::TextButton::buttonOnColourId, accent.withAlpha (0.35f));
    setColour (juce::TextButton::textColourOffId, text);
    setColour (juce::TextButton::textColourOnId, text);
    setColour (juce::ToggleButton::textColourId, text);
    setColour (juce::ToggleButton::tickColourId, accent);
    setColour (juce::TabbedComponent::backgroundColourId, panel);
    setColour (juce::TabbedComponent::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::TooltipWindow::backgroundColourId, panel.brighter (0.1f));
    setColour (juce::TooltipWindow::textColourId, text);
    setColour (juce::TooltipWindow::outlineColourId, outline);
    setColour (juce::TextEditor::backgroundColourId, panelLight);
    setColour (juce::TextEditor::textColourId, text);
    setColour (juce::TextEditor::outlineColourId, outline);
    setColour (juce::AlertWindow::backgroundColourId, panel);
    setColour (juce::AlertWindow::textColourId, text);
    setColour (juce::ScrollBar::thumbColourId, outline);
}

void LookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float start, float end,
                                    juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (3.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) / 2.0f;
    const auto centre = bounds.getCentre();
    const float lineW = juce::jmax (2.5f, radius * 0.16f);
    const float arcR = radius - lineW * 0.5f;
    const float angle = start + pos * (end - start);
    const bool enabled = slider.isEnabled();

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, start, end, true);
    g.setColour (panelLight.brighter (0.05f));
    g.strokePath (track, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Bipolar ranges (pan, ±dB) fill from the centre.
    const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0
                         && std::abs (slider.getMinimum() + slider.getMaximum()) < 1.0e-6;
    const float from = bipolar ? (start + end) * 0.5f : start;
    juce::Path value;
    value.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, juce::jmin (from, angle), juce::jmax (from, angle), true);
    g.setColour (enabled ? accent : textDim);
    g.strokePath (value, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    g.setColour (panel.brighter (0.08f));
    g.fillEllipse (juce::Rectangle<float> (radius * 1.1f, radius * 1.1f).withCentre (centre));
    const auto tip = centre.getPointOnCircumference (radius * 0.52f, angle);
    g.setColour (text);
    g.drawLine ({ centre.getPointOnCircumference (radius * 0.18f, angle), tip }, 2.0f);
}

void LookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& colour, bool highlighted,
                                        bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    auto c = b.getToggleState() ? accent.withAlpha (0.35f) : colour;
    if (down)
        c = c.brighter (0.2f);
    else if (highlighted)
        c = c.brighter (0.08f);
    g.setColour (c);
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (outline);
    g.drawRoundedRectangle (r, 4.0f, 1.0f);
}

void LookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool)
{
    auto r = b.getLocalBounds().toFloat();
    const float box = juce::jmin (16.0f, r.getHeight() - 4.0f);
    auto tick = r.removeFromLeft (box + 6.0f).withSizeKeepingCentre (box, box);
    g.setColour (b.getToggleState() ? accent : panelLight.brighter (highlighted ? 0.15f : 0.0f));
    g.fillRoundedRectangle (tick, 3.0f);
    g.setColour (outline);
    g.drawRoundedRectangle (tick, 3.0f, 1.0f);
    if (b.getToggleState())
    {
        g.setColour (background);
        juce::Path p;
        p.startNewSubPath (tick.getX() + box * 0.22f, tick.getCentreY());
        p.lineTo (tick.getX() + box * 0.42f, tick.getBottom() - box * 0.25f);
        p.lineTo (tick.getRight() - box * 0.2f, tick.getY() + box * 0.25f);
        g.strokePath (p, juce::PathStrokeType (2.0f));
    }
    g.setColour (b.isEnabled() ? text : textDim);
    g.setFont (juce::FontOptions (13.0f));
    g.drawFittedText (b.getButtonText(), r.toNearestInt(), juce::Justification::centredLeft, 2);
}

void LookAndFeel::drawTabButton (juce::TabBarButton& button, juce::Graphics& g, bool isMouseOver, bool)
{
    const auto r = button.getLocalBounds().toFloat();
    const bool front = button.isFrontTab();
    g.setColour (front ? panel : (isMouseOver ? panelLight : background));
    g.fillRect (r);
    if (front)
    {
        g.setColour (accent);
        g.fillRect (r.withHeight (2.0f));
    }
    g.setColour (front ? text : textDim);
    g.setFont (juce::FontOptions (13.0f, front ? juce::Font::bold : juce::Font::plain));
    g.drawText (button.getButtonText(), r, juce::Justification::centred);
}

int LookAndFeel::getTabButtonBestWidth (juce::TabBarButton& button, int)
{
    return juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (13.0f, juce::Font::bold), button.getButtonText()) + 26;
}

} // namespace theme
