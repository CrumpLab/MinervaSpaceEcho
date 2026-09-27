#include "MemoryMatrix.h"

#include "Theme.h"

#include <cmath>

namespace {

template <typename Value>
void drawAddressImpl (juce::Graphics& g, juce::Rectangle<float> area, Value valueAt)
{
    g.setColour (theme::background);
    g.fillRect (area);
    const float cw = area.getWidth() / mse::kSlots;
    const float ch = area.getHeight() / mse::kBands;
    for (int s = 0; s < mse::kSlots; ++s)
        for (int b = 0; b < mse::kBands; ++b)
        {
            g.setColour (valueAt (s * mse::kBands + b));
            g.fillRect (area.getX() + s * cw, area.getBottom() - (b + 1) * ch, cw + 0.5f, ch + 0.5f);
        }
    g.setColour (theme::outline);
    g.drawRect (area, 1.0f);
}

juce::String traceText (const mse::TraceView& r)
{
    juce::String s;
    s << "Trace #" << static_cast<juce::int64> (r.serial) << (r.clamped ? "  (clamped)" : "") << "\n"
      << "age " << r.age << (r.age == 1 ? " trace" : " traces") << ", " << juce::String (r.seconds, 2) << " s\n"
      << "activation " << juce::String (r.activation, 3) << "\n"
      << "level " << juce::String (20.0 * std::log10 (r.rms + 1.0e-9), 1) << " dB, strength "
      << juce::String (20.0 * std::log10 (r.strength + 1.0e-9), 1) << " dB\n"
      << "generation " << r.generation << (r.generation == 0 ? " (heard)" : " (echo)") << ", merged "
      << r.mergeCount << "x, used " << juce::String (r.useCount, 2);
    return s;
}

} // namespace

void drawAddress (juce::Graphics& g, juce::Rectangle<float> area, const float* values)
{
    drawAddressImpl (g, area, [values] (int i) { return theme::heat (values[i]); });
}

void drawAddress (juce::Graphics& g, juce::Rectangle<float> area, const int8_t* q)
{
    drawAddressImpl (g, area, [q] (int i) { return theme::heat (static_cast<float> (q[i]) / mse::MemoryView::kThumbScale); });
}

// ---- MemoryMatrix -------------------------------------------------------------------

MemoryMatrix::MemoryMatrix()
{
    setOpaque (true);
}

juce::Rectangle<float> MemoryMatrix::heatArea() const noexcept
{
    return getLocalBounds().toFloat().withTrimmedLeft (static_cast<float> (kLockW + kActW + kHeadsW + 3 * kGap));
}

int MemoryMatrix::displayRows() const noexcept
{
    // Zoom to fit: a sparsely filled memory gets tall rows; the rest of the
    // height stands for the free slots.
    return juce::jlimit (juce::jmin (capacity, 24), juce::jmax (1, capacity), count);
}

float MemoryMatrix::rowHeight() const noexcept
{
    return static_cast<float> (getHeight()) / static_cast<float> (displayRows());
}

int MemoryMatrix::rowAt (float y) const noexcept
{
    return juce::jlimit (0, juce::jmax (0, displayRows() - 1), static_cast<int> (y / rowHeight()));
}

void MemoryMatrix::resized()
{
    heatKeys.clear(); // image size follows the height
}

void MemoryMatrix::update (const mse::MemoryView& v)
{
    count = v.count;
    capacity = juce::jmax (1, v.capacity);
    clampLimit = v.clampLimit;
    rows.assign (v.rows.begin(), v.rows.begin() + v.count);

    maxAct = 0.0f;
    maxPlay = 0.0f;
    for (const auto& r : rows)
    {
        maxAct = juce::jmax (maxAct, std::abs (r.activation));
        for (float p : r.play)
            maxPlay = juce::jmax (maxPlay, p);
    }

    // The address image has at most one row per pixel; each image row shows
    // the trace at that height.
    // With sequence context each row is [n-1 | n]: the context half first.
    const int rowsShown = displayRows();
    const int imageRows = juce::jlimit (1, rowsShown, juce::jmax (1, getHeight()));
    const int cols = v.sequence ? 2 * mse::kFeatureSize : mse::kFeatureSize;
    sequence = v.sequence;
    if (! heat.isValid() || heat.getHeight() != imageRows || heat.getWidth() != cols)
    {
        heat = juce::Image (juce::Image::ARGB, cols, imageRows, true);
        heatKeys.clear();
    }
    heatKeys.resize (static_cast<size_t> (imageRows), ~uint64_t { 0 } - 1);
    {
        juce::Image::BitmapData bits (heat, juce::Image::BitmapData::writeOnly);
        const auto empty = theme::panel.getPixelARGB();
        for (int y = 0; y < imageRows; ++y)
        {
            const int row = static_cast<int> (static_cast<int64_t> (y) * rowsShown / imageRows);
            const uint64_t key = row < count ? v.thumbKey (row) : ~uint64_t { 0 };
            if (heatKeys[static_cast<size_t> (y)] == key)
                continue;
            heatKeys[static_cast<size_t> (y)] = key;
            auto* line = reinterpret_cast<juce::PixelARGB*> (bits.getLinePointer (y));
            if (row >= count)
            {
                std::fill (line, line + cols, empty);
                continue;
            }
            if (sequence)
            {
                const int8_t* c = v.contextThumb (row);
                for (int j = 0; j < mse::kFeatureSize; ++j)
                    line[j] = theme::heatQ (c[j]);
                line += mse::kFeatureSize;
            }
            const int8_t* t = v.thumb (row);
            for (int j = 0; j < mse::kFeatureSize; ++j)
                line[j] = theme::heatQ (t[j]);
        }
    }
    repaint();
}

void MemoryMatrix::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);
    const auto heatRect = heatArea();
    const float rowH = rowHeight();

    if (heat.isValid())
    {
        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
        g.drawImage (heat, heatRect, juce::RectanglePlacement::stretchToFit);
    }
    // Slot separators.
    const int slots = sequence ? 2 * mse::kSlots : mse::kSlots;
    const float slotW = heatRect.getWidth() / static_cast<float> (slots);
    if (slotW >= 10.0f)
    {
        g.setColour (theme::background.withAlpha (0.55f));
        for (int s = 1; s < slots; ++s)
            g.fillRect (heatRect.getX() + s * slotW - 0.5f, heatRect.getY(), 1.0f, heatRect.getHeight());
    }
    if (sequence)
    {
        // Divider between the n-1 and n halves.
        g.setColour (theme::background);
        g.fillRect (heatRect.getCentreX() - 1.5f, heatRect.getY(), 3.0f, heatRect.getHeight());
    }

    const float lockX = 0.0f;
    const float actX = static_cast<float> (kLockW + kGap);
    const float headsX = actX + kActW + kGap;
    const float filledH = rowH * static_cast<float> (count);

    // Column backgrounds.
    g.setColour (theme::panel);
    g.fillRect (lockX, 0.0f, static_cast<float> (kLockW), filledH);
    g.fillRect (actX, 0.0f, static_cast<float> (kActW), filledH);
    g.fillRect (headsX, 0.0f, static_cast<float> (kHeadsW), filledH);

    auto drawRow = [&] (float y, float h, bool clamped, float act, const float* play) {
        if (clamped)
        {
            g.setColour (theme::accent);
            if (h >= 9.0f)
            {
                const auto r = juce::Rectangle<float> (lockX + 3.0f, y + 1.0f, kLockW - 6.0f, h - 2.0f)
                                   .withSizeKeepingCentre (8.0f, juce::jmin (9.0f, h - 2.0f));
                g.fillRoundedRectangle (r.withTrimmedTop (r.getHeight() * 0.4f), 1.5f);
                juce::Path shackle;
                shackle.addCentredArc (r.getCentreX(), r.getY() + r.getHeight() * 0.42f, 2.6f, r.getHeight() * 0.32f, 0.0f,
                                       -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi, true);
                g.strokePath (shackle, juce::PathStrokeType (1.4f));
            }
            else
            {
                g.fillRect (lockX + 4.0f, y, kLockW - 8.0f, juce::jmax (1.0f, h - (h > 3.0f ? 1.0f : 0.0f)));
            }
        }
        if (maxAct > 0.0f && std::abs (act) > 0.0f)
        {
            const float w = juce::jmin (1.0f, std::abs (act) / maxAct) * kActW;
            g.setColour (act > 0.0f ? theme::accent : theme::negative);
            g.fillRect (actX, y + (h > 3.0f ? 0.5f : 0.0f), w, juce::jmax (1.0f, h - (h > 3.0f ? 1.0f : 0.0f)));
        }
        if (maxPlay > 0.0f)
            for (int hd = 0; hd < mse::kNumHeads; ++hd)
                if (play[hd] > 0.0f)
                {
                    g.setColour (theme::headColour (hd).withAlpha (0.35f + 0.65f * juce::jmin (1.0f, play[hd] / maxPlay)));
                    g.fillRect (headsX + 3.0f + hd * 10.0f, y + (h > 3.0f ? 0.5f : 0.0f), 8.0f,
                                juce::jmax (1.0f, h - (h > 3.0f ? 1.0f : 0.0f)));
                }
    };

    if (rowH >= 1.5f)
    {
        for (int i = 0; i < count; ++i)
        {
            const auto& r = rows[static_cast<size_t> (i)];
            drawRow (i * rowH, rowH, r.clamped, r.activation, r.play);
        }
    }
    else
    {
        // More traces than pixels: each pixel line shows the strongest of its rows.
        for (int y = 0; y < static_cast<int> (std::ceil (filledH)); ++y)
        {
            const int a = static_cast<int> (y / rowH), b = juce::jmin (count, static_cast<int> ((y + 1) / rowH) + 1);
            bool clamped = false;
            float act = 0.0f, play[mse::kNumHeads] {};
            for (int i = a; i < b; ++i)
            {
                const auto& r = rows[static_cast<size_t> (i)];
                clamped = clamped || r.clamped;
                if (std::abs (r.activation) > std::abs (act))
                    act = r.activation;
                for (int hd = 0; hd < mse::kNumHeads; ++hd)
                    play[hd] = juce::jmax (play[hd], r.play[hd]);
            }
            drawRow (static_cast<float> (y), 1.0f, clamped, act, play);
        }
    }

    if (rowH >= 3.0f && count > 0)
    {
        // Row separators keep tall rows readable.
        g.setColour (theme::background.withAlpha (rowH >= 6.0f ? 0.8f : 0.4f));
        for (int i = 1; i <= count; ++i)
            g.fillRect (0.0f, i * rowH - 0.5f, static_cast<float> (getWidth()), rowH >= 6.0f ? 1.0f : 0.5f);
    }

    auto outlineRow = [&] (int i, juce::Colour c, float thickness) {
        auto r = juce::Rectangle<float> (0.0f, i * rowH, static_cast<float> (getWidth()), juce::jmax (2.0f, rowH));
        g.setColour (c);
        g.drawRect (r.expanded (0.0f, rowH < 2.0f ? 1.0f : 0.0f), thickness);
    };
    for (int i = 0; i < count; ++i)
    {
        if (rows[static_cast<size_t> (i)].serial == selected)
            outlineRow (i, theme::text, 1.5f);
        if (auditioning != 0 && rows[static_cast<size_t> (i)].serial == auditioning)
            outlineRow (i, theme::head2, 2.0f);
    }
    if (hoverRow >= 0 && hoverRow < count)
        outlineRow (hoverRow, theme::text.withAlpha (0.35f), 1.0f);

    if (count > 0 && count < capacity && rowH * (displayRows() - count) >= 16.0f)
    {
        g.setColour (theme::textDim);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (juce::String (capacity - count) + " free", heatRect.withTop (filledH + 4.0f).withHeight (16.0f).reduced (6.0f, 0.0f),
                    juce::Justification::centredLeft);
    }

    if (count == 0)
    {
        g.setColour (theme::textDim);
        g.setFont (juce::FontOptions (14.0f));
        g.drawFittedText ("Memory is empty.\nPlay something, or drop audio files here to seed it.",
                          heatRect.toNearestInt().reduced (10), juce::Justification::centred, 3);
    }
}

void MemoryMatrix::mouseDown (const juce::MouseEvent& e)
{
    const int row = rowAt (e.position.y);
    if (row >= count)
        return;
    const auto& r = rows[static_cast<size_t> (row)];
    if (e.position.x < kLockW + kGap / 2)
    {
        if (onLock)
            onLock (r.serial, ! r.clamped);
        return;
    }
    if (e.mods.isAltDown())
    {
        // Alt-click: listen to the trace (again to stop).
        if (onAudition)
            onAudition (auditioning == r.serial ? 0 : r.serial);
        return;
    }
    selected = selected == r.serial ? 0 : r.serial;
    if (onSelect)
        onSelect (selected);
    repaint();
}

void MemoryMatrix::mouseMove (const juce::MouseEvent& e)
{
    const int row = rowAt (e.position.y);
    const int h = row < count ? row : -1;
    if (h != hoverRow)
    {
        hoverRow = h;
        repaint();
    }
}

void MemoryMatrix::mouseExit (const juce::MouseEvent&)
{
    hoverRow = -1;
    repaint();
}

juce::String MemoryMatrix::getTooltip()
{
    if (hoverRow < 0 || hoverRow >= count)
        return {};
    auto text = traceText (rows[static_cast<size_t> (hoverRow)]);
    text << "\n\nClick to inspect; Alt-click to listen; click the lock column to clamp or unclamp.";
    return text;
}

// ---- MemorySidePanel -------------------------------------------------------------------

MemorySidePanel::MemorySidePanel()
{
    addChildComponent (lockButton);
    addChildComponent (deleteButton);
    lockButton.onClick = [this] {
        if (hasSelection && onLock)
            onLock (sel.serial, ! sel.clamped);
    };
    deleteButton.onClick = [this] {
        if (hasSelection && onDelete)
            onDelete (sel.serial);
    };
    lockButton.setTooltip ("Clamped (locked) traces are never replaced when memory is full");
    deleteButton.setTooltip ("Remove this trace from memory");

    addChildComponent (auditionButton);
    addChildComponent (pairButton);
    addChildComponent (loopToggle);
    auditionButton.onClick = [this] {
        if (hasSelection && onAudition)
            onAudition (auditioning == sel.serial ? 0 : sel.serial, loopToggle.getToggleState() ? 1 : 0);
    };
    pairButton.onClick = [this] {
        if (hasSelection && onAudition)
            onAudition (sel.serial, 2);
    };
    auditionButton.setTooltip ("Listen to this trace on its own (the plug-in's output is muted while it plays). Alt-click a row does the same.");
    pairButton.setTooltip ("Listen to the segment before this trace (if it is still in memory), then the trace: its [n-1 | n]");
    loopToggle.setTooltip ("Repeat the audition until stopped");
}

void MemorySidePanel::setAuditioning (uint64_t serial)
{
    if (serial != auditioning)
    {
        auditioning = serial;
        updateAuditionButtons();
    }
}

void MemorySidePanel::updateAuditionButtons()
{
    auditionButton.setVisible (hasSelection);
    loopToggle.setVisible (hasSelection);
    pairButton.setVisible (hasSelection && sequence);
    auditionButton.setButtonText (hasSelection && auditioning != 0 ? "Stop" : "Audition");
    auditionButton.setToggleState (hasSelection && auditioning == sel.serial, juce::dontSendNotification);
}

void MemorySidePanel::update (const mse::MemoryView& v, uint64_t selectedSerial)
{
    heard = v.heard;
    heardBefore = v.heardBefore;
    hasEcho = v.echoContent (echo);
    v.echoContextContent (echoContext);
    sequence = v.sequence;
    predicting = v.chain || v.contextCue == static_cast<int> (mse::ContextCue::PredictNext);
    intensity = v.intensity;
    maxActivation = v.maxActivation;
    intensityShown += (juce::jlimit (0.0f, 1.0f, maxActivation) - intensityShown) * 0.35f;

    hasSelection = false;
    for (int i = 0; i < v.count && selectedSerial != 0; ++i)
        if (v.rows[static_cast<size_t> (i)].serial == selectedSerial)
        {
            hasSelection = true;
            sel = v.rows[static_cast<size_t> (i)];
            std::copy (v.thumb (i), v.thumb (i) + mse::kFeatureSize, selThumb.begin());
            std::copy (v.contextThumb (i), v.contextThumb (i) + mse::kFeatureSize, selContextThumb.begin());
            break;
        }
    lockButton.setVisible (hasSelection);
    deleteButton.setVisible (hasSelection);
    updateAuditionButtons();
    lockButton.setButtonText (hasSelection && sel.clamped ? "Unclamp" : "Clamp");
    repaint();
}

void MemorySidePanel::resized()
{
    auto r = getLocalBounds().reduced (8, 6);
    auto top = r.removeFromTop (juce::jmin (116, r.getHeight() / 3));
    heardArea = top.removeFromLeft (top.getWidth() / 2).reduced (0, 0).withTrimmedRight (4);
    echoArea = top.withTrimmedLeft (4);
    r.removeFromTop (6);
    meterArea = r.removeFromTop (34);
    r.removeFromTop (8);
    auto buttons = r.removeFromBottom (26);
    lockButton.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2).reduced (0, 0).withTrimmedRight (3));
    deleteButton.setBounds (buttons.withTrimmedLeft (3));
    r.removeFromBottom (4);
    auto listen = r.removeFromBottom (26);
    const int third = listen.getWidth() / 3;
    auditionButton.setBounds (listen.removeFromLeft (third).withTrimmedRight (3));
    pairButton.setBounds (listen.removeFromRight (third).withTrimmedLeft (3));
    loopToggle.setBounds (listen.reduced (4, 0));
    r.removeFromBottom (6);
    selArea = r;
}

void MemorySidePanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));

    auto caption = [&] (juce::Rectangle<int> area, const juce::String& text) {
        g.setColour (theme::textDim);
        g.drawText (text, area.removeFromTop (14), juce::Justification::centredLeft);
        return area;
    };
    // Two halves side by side, [n-1 | n].
    auto drawPair = [&] (juce::Rectangle<int> area, const float* before, const float* now) {
        auto left = area.removeFromLeft (area.getWidth() / 2);
        drawAddress (g, left.withTrimmedRight (2).toFloat(), before);
        drawAddress (g, area.withTrimmedLeft (2).toFloat(), now);
    };
    juce::Rectangle<int> heardImg, echoImg;
    if (sequence)
    {
        // Stacked, full width: [n-1 | n] for what was heard, and for the echo.
        auto both = heardArea.getUnion (echoArea);
        auto top = both.removeFromTop (both.getHeight() / 2);
        heardImg = caption (top.withTrimmedBottom (3), "HEARD   n-1 | n");
        echoImg = caption (both, predicting ? "ECHO   context | expected next" : "ECHO   n-1 | n");
        drawPair (heardImg, heardBefore.data(), heard.data());
    }
    else
    {
        heardImg = caption (heardArea, "HEARD");
        echoImg = caption (echoArea, "ECHO");
        drawAddress (g, heardImg.toFloat(), heard.data());
    }
    if (hasEcho)
    {
        if (sequence)
            drawPair (echoImg, echoContext.data(), echo.data());
        else
            drawAddress (g, echoImg.toFloat(), echo.data());
    }
    else
    {
        g.setColour (theme::background);
        g.fillRect (echoImg);
        g.setColour (theme::textDim);
        g.setFont (juce::FontOptions (11.0f));
        g.drawFittedText ("no echo", echoImg, juce::Justification::centred, 1);
    }

    // Familiarity: the strongest activation (0..1), and MINERVA's intensity.
    auto m = meterArea;
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.setColour (theme::textDim);
    auto label = m.removeFromTop (14);
    g.drawText ("FAMILIARITY", label, juce::Justification::centredLeft);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText ("intensity " + juce::String (intensity, 2), label, juce::Justification::centredRight);
    auto bar = m.reduced (0, 3).toFloat();
    g.setColour (theme::background);
    g.fillRoundedRectangle (bar, 3.0f);
    g.setColour (theme::accent);
    g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * intensityShown), 3.0f);

    // Selected trace.
    auto s = selArea;
    if (! hasSelection)
    {
        g.setColour (theme::textDim);
        g.setFont (juce::FontOptions (12.0f));
        g.drawFittedText ("Click a row to inspect a trace.\nClick its lock column to clamp it.\n\n"
                          "Rows: oldest at the top. Bars: activation. Dots: heads playing it (1 amber, 2 teal, 3 violet).",
                          s, juce::Justification::topLeft, 8);
        return;
    }
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.setColour (sel.clamped ? theme::accent : theme::textDim);
    g.drawText ("TRACE #" + juce::String (static_cast<juce::int64> (sel.serial)) + (sel.clamped ? "  CLAMPED" : ""),
                s.removeFromTop (14), juce::Justification::centredLeft);
    const int imgH = juce::jmin (64, s.getHeight() / 2);
    if (sequence)
    {
        auto img = s.removeFromTop (imgH);
        auto left = img.removeFromLeft (img.getWidth() / 2);
        drawAddress (g, left.withTrimmedRight (2).toFloat(), selContextThumb.data());
        drawAddress (g, img.withTrimmedLeft (2).toFloat(), selThumb.data());
    }
    else
    {
        drawAddress (g, s.removeFromTop (imgH).toFloat(), selThumb.data());
    }
    s.removeFromTop (4);
    g.setColour (theme::text);
    g.setFont (juce::FontOptions (12.0f));
    juce::String info;
    info << "age " << sel.age << ", " << juce::String (sel.seconds, 2) << " s, activation " << juce::String (sel.activation, 3)
         << "\nlevel " << juce::String (20.0 * std::log10 (sel.rms + 1.0e-9), 1) << " dB, strength "
         << juce::String (20.0 * std::log10 (sel.strength + 1.0e-9), 1) << " dB"
         << "\ngeneration " << sel.generation << ", merged " << sel.mergeCount << "x";
    g.drawFittedText (info, s, juce::Justification::topLeft, 4);
}
