#include "ProbePanel.h"

#include "Theme.h"

#include <algorithm>
#include <cmath>

namespace {

const char* const kSetShort[mse::kNumSets] = { "Sp", "PC", "Pi", "Ti", "Rh" };

juce::String twoPlaces (float v)
{
    // ".98", "-.12", "1.0": three characters where possible, so the columns fit.
    if (v >= 0.995f)
        return "1.0";
    if (v <= -0.995f)
        return "-1";
    auto s = juce::String (v, 2);
    return s.replace ("0.", ".");
}

} // namespace

ProbePanel::ProbePanel()
{
    for (auto* b : { static_cast<juce::Component*> (&playButton), static_cast<juce::Component*> (&compareButton),
                     static_cast<juce::Component*> (&closeButton), static_cast<juce::Component*> (&loopToggle),
                     static_cast<juce::Component*> (&fullToggle), static_cast<juce::Component*> (&selfToggle),
                     static_cast<juce::Component*> (&list) })
        addAndMakeVisible (b);

    playButton.onClick = [this] {
        if (! onCommand)
            return;
        if (playing)
            onCommand (mse::Command::StopAudition, 0);
        else
            onCommand (mse::Command::ProbePlay, (loopToggle.getToggleState() ? 1u : 0u) | (fullToggle.getToggleState() ? 2u : 0u));
    };
    compareButton.onClick = [this] {
        if (onCommand)
            onCommand (mse::Command::ProbeCompare, next ? 0 : 1);
    };
    selfToggle.onClick = [this] {
        if (onCommand)
            onCommand (mse::Command::ProbeIncludeSelf, selfToggle.getToggleState() ? 1 : 0);
    };
    closeButton.onClick = [this] {
        if (onCommand)
        {
            if (playing)
                onCommand (mse::Command::StopAudition, 0);
            onCommand (mse::Command::ClearProbe, 0);
        }
    };
    playButton.setTooltip ("Play the echo this probe produces, soloed (the plug-in's output is muted while it plays)");
    loopToggle.setTooltip ("Repeat the probe's echo until stopped");
    fullToggle.setTooltip ("Raw: the activation-weighted mix of the traces' audio. Full path: the same through the echo's "
                           "tone, tape (wow, flutter, drive, hiss), head 1 and echo level, and spring");
    compareButton.setTooltip ("n vs n: which traces are like the probe (recall). n vs n-1: which traces came after "
                              "something like the probe (what memory expects next)");
    selfToggle.setTooltip ("Let the probe trace answer itself (it always matches itself at 1)");
    closeButton.setTooltip ("End the probe");

    list.setRowHeight (16);
    list.setColour (juce::ListBox::backgroundColourId, theme::background);
    list.setColour (juce::ListBox::outlineColourId, theme::outline);
    list.setOutlineThickness (1);
}

void ProbePanel::setAuditioning (uint64_t serial)
{
    if (serial != auditioning)
    {
        auditioning = serial;
        list.repaint();
    }
}

void ProbePanel::update (const mse::MemoryView& v)
{
    probeSerial = v.probeSerial;
    playing = v.probePlaying;
    next = v.probeNext;
    includeSelf = v.probeIncludeSelf;
    sequence = v.sequence;
    shownSet = v.shownSet;
    count = v.probeCount;
    intensity = v.probeIntensity;

    // The probe first, then every trace by how strongly it answers (then by
    // similarity, for those too weak to count).
    ranked.clear();
    totalActivation = 0.0f;
    Row probeRow;
    for (int i = 0; i < v.count; ++i)
    {
        const auto& r = v.rows[static_cast<size_t> (i)];
        totalActivation += std::abs (r.probe);
        if (r.serial == probeSerial)
        {
            probeRow.trace = r;
            probeRow.isProbe = true;
        }
        if (r.serial != probeSerial || includeSelf)
            ranked.push_back ({ r, false, 0 });
    }
    std::stable_sort (ranked.begin(), ranked.end(), [] (const Row& a, const Row& b) {
        const float aa = std::abs (a.trace.probe), ab = std::abs (b.trace.probe);
        return aa != ab ? aa > ab : a.trace.probeSim > b.trace.probeSim;
    });
    for (size_t k = 0; k < ranked.size(); ++k)
        ranked[k].rank = static_cast<int> (k) + 1;
    if (probeRow.isProbe)
        ranked.insert (ranked.begin(), probeRow);

    playButton.setButtonText (playing ? "Stop" : "Play echo");
    compareButton.setButtonText (next ? "n vs n-1" : "n vs n");
    compareButton.setVisible (sequence || next);
    selfToggle.setToggleState (includeSelf, juce::dontSendNotification);
    list.updateContent();
    list.repaint();
    repaint (titleArea.getUnion (headerArea));
}

void ProbePanel::resized()
{
    auto r = getLocalBounds();
    titleArea = r.removeFromTop (30);
    auto row1 = r.removeFromTop (24);
    playButton.setBounds (row1.removeFromLeft (84).withTrimmedRight (4));
    loopToggle.setBounds (row1.removeFromLeft (62));
    fullToggle.setBounds (row1);
    r.removeFromTop (4);
    auto row2 = r.removeFromTop (24);
    closeButton.setBounds (row2.removeFromRight (56).withTrimmedLeft (4));
    compareButton.setBounds (row2.removeFromLeft (76).withTrimmedRight (4));
    selfToggle.setBounds (row2);
    r.removeFromTop (6);
    headerArea = r.removeFromTop (14);
    list.setBounds (r);
}

void ProbePanel::layoutColumns (juce::Rectangle<int> area, std::array<juce::Rectangle<int>, 9>& cols) const
{
    // rank | trace | activation % | S | Sp PC Pi Ti Rh
    area = area.withTrimmedLeft (3).withTrimmedRight (3);
    cols[0] = area.removeFromLeft (18);
    cols[1] = area.removeFromLeft (34);
    cols[2] = area.removeFromLeft (32);
    cols[3] = area.removeFromLeft (28);
    const int w = area.getWidth() / mse::kNumSets;
    for (int s = 0; s < mse::kNumSets; ++s)
        cols[static_cast<size_t> (4 + s)] = s == mse::kNumSets - 1 ? area : area.removeFromLeft (w);
}

void ProbePanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    auto t = titleArea;
    g.setColour (theme::probe);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText ("PROBE #" + juce::String (static_cast<juce::int64> (probeSerial)) + "   "
                    + juce::String (mse::addressSetName (shownSet)).toUpperCase() + (next ? "   n vs n-1" : "   n vs n"),
                t.removeFromTop (14), juce::Justification::centredLeft);
    g.setColour (theme::textDim);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText (juce::String (count) + (count == 1 ? " trace answers" : " traces answer") + ", intensity "
                    + juce::String (intensity, 2),
                t, juce::Justification::centredLeft);

    std::array<juce::Rectangle<int>, 9> cols;
    layoutColumns (headerArea, cols);
    g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
    const char* const heads[4] = { "", "#", "act", "S" };
    for (int c = 0; c < 4; ++c)
    {
        g.setColour (theme::textDim);
        g.drawText (heads[c], cols[static_cast<size_t> (c)], juce::Justification::centredLeft);
    }
    for (int s = 0; s < mse::kNumSets; ++s)
    {
        g.setColour (s == shownSet ? theme::probe : theme::textDim);
        g.drawText (kSetShort[s], cols[static_cast<size_t> (4 + s)], juce::Justification::centredLeft);
    }
}

void ProbePanel::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool)
{
    if (row < 0 || row >= static_cast<int> (ranked.size()))
        return;
    const auto& r = ranked[static_cast<size_t> (row)];
    const auto area = juce::Rectangle<int> (0, 0, width, height);
    if (r.trace.serial == auditioning)
        g.fillAll (theme::head2.withAlpha (0.25f));
    else if (r.isProbe)
        g.fillAll (theme::probe.withAlpha (0.15f));
    else if (row % 2 == 0)
        g.fillAll (theme::panel.withAlpha (0.5f));

    std::array<juce::Rectangle<int>, 9> cols;
    layoutColumns (area, cols);
    g.setFont (juce::FontOptions (10.5f));
    const bool answers = r.trace.probe != 0.0f;
    const auto ink = r.isProbe ? theme::probe : (answers ? theme::text : theme::textDim);
    g.setColour (ink);
    g.drawText (r.isProbe ? juce::String ("P") : juce::String (r.rank), cols[0], juce::Justification::centredLeft);
    g.drawText ("#" + juce::String (static_cast<juce::int64> (r.trace.serial)), cols[1], juce::Justification::centredLeft);
    juce::String act = "-";
    if (answers && totalActivation > 0.0f)
    {
        const float pct = 100.0f * std::abs (r.trace.probe) / totalActivation;
        act = pct < 1.0f ? juce::String ("<1%") : juce::String (juce::roundToInt (pct)) + "%";
        if (r.trace.probe < 0.0f)
            act = "-" + act;
    }
    g.drawText (r.isProbe && ! includeSelf ? juce::String ("probe") : act, cols[2], juce::Justification::centredLeft);
    g.drawText (twoPlaces (r.trace.probeSim), cols[3], juce::Justification::centredLeft);
    for (int s = 0; s < mse::kNumSets; ++s)
    {
        g.setColour (s == shownSet ? ink : ink.withMultipliedAlpha (0.7f));
        g.drawText (twoPlaces (r.trace.probeSims[static_cast<size_t> (s)]), cols[static_cast<size_t> (4 + s)],
                    juce::Justification::centredLeft);
    }
}

void ProbePanel::listBoxItemClicked (int row, const juce::MouseEvent&)
{
    if (row >= 0 && row < static_cast<int> (ranked.size()) && onAuditionRow)
        onAuditionRow (ranked[static_cast<size_t> (row)].trace.serial);
}

juce::String ProbePanel::getTooltipForRow (int row)
{
    if (row < 0 || row >= static_cast<int> (ranked.size()))
        return {};
    const auto& r = ranked[static_cast<size_t> (row)];
    juce::String s;
    s << (r.isProbe ? "The probe, trace #" : "Trace #") << static_cast<juce::int64> (r.trace.serial) << ": activation "
      << juce::String (r.trace.probe, 4) << ", similarity " << juce::String (r.trace.probeSim, 3) << "\n";
    for (int set = 0; set < mse::kNumSets; ++set)
        s << mse::addressSetName (set) << " " << juce::String (r.trace.probeSims[static_cast<size_t> (set)], 3)
          << (set + 1 < mse::kNumSets ? ", " : "");
    s << "\nClick to listen to it";
    return s;
}
