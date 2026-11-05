/*
  ------------------------------------------------------------------------------
    GainReductionMeter.h

    The gain a dynamics stage applies (a compressor's or a limiter's
    reduction, a transient shaper's cut or boost), drawn the way a hardware
    console does: a column of LED segments lit from a 0 dB origin towards the
    current gain, a held peak segment, a dB scale beside it and the held
    value written above.

    The range is any [minDb, maxDb] containing 0 dB: the default -30 to 0
    reads a compressor (segments light downwards from the top), -12 to +12 a
    transient shaper (down for a cut, up for a boost, from the middle).

    Push model: call setValue() from the editor's timer with the latest gain
    in dB (a fxme::VuMeter's getPeak(), say). The meter repaints itself only
    when what it shows changes; it owns no timer. The held peak (the gain
    farthest from 0 dB) stays for setPeakHoldMs(), then falls back at
    setPeakFallDbPerSecond().

    setActive (false) greys it, for a stage that is switched off. Pure
    display, depends only on JUCE. For a level meter with a threshold see
    fxme::DynamicsMeter; for a plain bar, fxme::VuMeterComponent.

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>

#include <cmath>
#include <vector>

namespace fxme
{

class GainReductionMeter : public juce::Component,
                           public juce::SettableTooltipClient
{
public:
    GainReductionMeter()
    {
        setRange (-30.0f, 0.0f);
    }

    /** The scale, in dB; it must contain 0 dB, the origin of the segments.
        Resets the scale ticks to the defaults for that range. */
    void setRange (float newMinDb, float newMaxDb)
    {
        minDb = juce::jmin (newMinDb, 0.0f);
        maxDb = juce::jmax (newMaxDb, 0.0f);
        if (maxDb - minDb < 1.0f)
            minDb = maxDb - 1.0f;
        ticks.clear();
        for (float t : { 24.0f, 18.0f, 12.0f, 6.0f, 3.0f, 0.0f, -3.0f, -6.0f, -10.0f, -20.0f, -30.0f, -40.0f, -60.0f })
            if (t >= minDb && t <= maxDb)
                ticks.push_back (t);
        repaint();
    }

    /** The dB values marked on the scale (inside the range). */
    void setScaleTicks (std::vector<float> newTicks)
    {
        ticks.clear();
        for (float t : newTicks)
            if (t >= minDb && t <= maxDb)
                ticks.push_back (t);
        repaint();
    }

    /** The current gain, in dB (negative: a reduction). */
    void setValue (float gainDb)
    {
        if (! std::isfinite (gainDb))
            gainDb = 0.0f;
        gainDb = juce::jlimit (minDb, maxDb, gainDb);

        const double now = juce::Time::getMillisecondCounterHiRes();
        const double elapsed = lastUpdateMs > 0.0 ? (now - lastUpdateMs) / 1000.0 : 0.0;
        lastUpdateMs = now;

        // The held peak: the gain farthest from 0 dB, kept for a while, then
        // falling back towards the current gain.
        if (std::abs (gainDb) >= std::abs (peakDb) || (gainDb > 0.0f) != (peakDb > 0.0f))
        {
            peakDb = gainDb;
            peakSinceMs = now;
        }
        else if (now - peakSinceMs > peakHoldMs)
        {
            const float fall = (float) (fallDbPerSecond * elapsed);
            peakDb = peakDb < 0.0f ? juce::jmin (gainDb, peakDb + fall) : juce::jmax (gainDb, peakDb - fall);
            if (std::abs (peakDb - gainDb) < 1.0e-3f)
                peakDb = gainDb;
        }

        if (std::abs (gainDb - valueDb) > 0.01f || std::abs (peakDb - shownPeakDb) > 0.01f)
        {
            valueDb = gainDb;
            shownPeakDb = peakDb;
            repaint();
        }
    }

    /** The lit segments' colour. */
    void setMeterColour (juce::Colour c)        { if (c != colour) { colour = c; repaint(); } }

    /** False greys the meter (the stage is switched off). */
    void setActive (bool shouldBeActive)        { if (shouldBeActive != active) { active = shouldBeActive; repaint(); } }

    void setPeakHoldMs (double ms) noexcept      { peakHoldMs = juce::jmax (0.0, ms); }
    void setPeakFallDbPerSecond (double dbPerSecond) noexcept   { fallDbPerSecond = juce::jmax (0.1, dbPerSecond); }

    /** The scale's labels beside the segments, when there is room (default on). */
    void setShowScale (bool shouldShow)         { showScale = shouldShow; repaint(); }

    /** The held value above the segments (default on). */
    void setShowReadout (bool shouldShow)       { showReadout = shouldShow; repaint(); }

    void paint (juce::Graphics& g) override
    {
        const auto layout = computeLayout();
        const auto lit = active ? colour : colour.withSaturation (colour.getSaturation() * 0.3f).withMultipliedAlpha (0.5f);
        const auto text = juce::Colours::white.withAlpha (active ? 0.75f : 0.35f);

        // The held value, above.
        if (! layout.readout.isEmpty())
        {
            g.setColour (text);
            g.setFont (juce::Font (juce::FontOptions (juce::jmin (12.0f, layout.readout.getHeight() * 0.85f), juce::Font::bold)));
            g.drawText (formatDb (shownPeakDb), layout.readout, juce::Justification::centred, false);
        }

        // The column: a recessed slot, then the segments.
        const auto column = layout.column;
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillRoundedRectangle (column.expanded (1.5f), 2.5f);

        const int count = layout.segments;
        if (count > 0)
        {
            const float pitch = column.getHeight() / (float) count;
            const float gap = juce::jmin (1.5f, pitch * 0.3f);
            const float origin = valueToY (0.0f, column);
            const float valueY = valueToY (valueDb, column);
            const float peakY = valueToY (shownPeakDb, column);

            for (int i = 0; i < count; ++i)
            {
                const float top = column.getY() + (float) i * pitch;
                const auto segment = juce::Rectangle<float> (column.getX(), top + gap * 0.5f, column.getWidth(), pitch - gap);
                const float centre = segment.getCentreY();

                // Lit between the origin and the value; the held peak's segment
                // brighter.
                const bool isLit = (centre >= juce::jmin (origin, valueY) && centre <= juce::jmax (origin, valueY))
                                   && std::abs (valueY - origin) > pitch * 0.25f;
                const bool isPeak = std::abs (shownPeakDb) > 0.05f && top <= peakY && peakY < top + pitch;

                if (isPeak)
                    g.setColour (lit.brighter (0.6f));
                else if (isLit)
                    g.setColour (lit);
                else
                    g.setColour (lit.withMultipliedAlpha (0.13f));
                g.fillRoundedRectangle (segment, juce::jmin (1.5f, segment.getHeight() * 0.4f));
            }

            // The 0 dB origin, across the column.
            g.setColour (juce::Colours::white.withAlpha (active ? 0.6f : 0.3f));
            g.fillRect (juce::Rectangle<float> (column.getX() - 2.0f, origin - 0.5f, column.getWidth() + 4.0f, 1.0f));
        }

        // The scale: ticks against the column, labels beside them.
        if (! layout.scale.isEmpty())
        {
            const float size = juce::jlimit (7.5f, 10.0f, layout.scale.getWidth() * 0.45f);
            g.setFont (juce::Font (juce::FontOptions (size)));
            float lastLabelY = -1.0e9f;
            for (float t : ticks)
            {
                const float y = valueToY (t, column);
                g.setColour (text.withMultipliedAlpha (0.7f));
                g.fillRect (juce::Rectangle<float> (layout.scale.getX(), y - 0.5f, 3.0f, 1.0f));
                if (std::abs (y - lastLabelY) < size + 1.0f)
                    continue;   // too close to the previous label
                lastLabelY = y;
                g.setColour (text.withMultipliedAlpha (0.85f));
                g.drawText (formatTick (t), juce::Rectangle<float> (layout.scale.getX() + 4.0f, y - size * 0.6f,
                                                                    layout.scale.getWidth() - 4.0f, size * 1.2f),
                            juce::Justification::centredLeft, false);
            }
        }
    }

    void resized() override     { repaint(); }

private:
    struct Layout
    {
        juce::Rectangle<float> readout, column, scale;
        int segments = 0;
    };

    Layout computeLayout() const
    {
        Layout l;
        auto area = getLocalBounds().toFloat().reduced (1.0f, 2.0f);
        if (showReadout && area.getHeight() > 60.0f)
            l.readout = area.removeFromTop (juce::jmin (16.0f, area.getHeight() * 0.12f));
        area.removeFromTop (3.0f);
        area.removeFromBottom (3.0f);

        // The column (at most 14 px wide) and the scale beside it, as one
        // group centred in the meter; the scale only when there is room.
        const float scaleWidth = showScale && area.getWidth() >= 26.0f ? juce::jmin (22.0f, area.getWidth() * 0.5f) : 0.0f;
        const float columnWidth = juce::jlimit (2.0f, 14.0f, area.getWidth() - scaleWidth - 3.0f);
        const float groupWidth = columnWidth + (scaleWidth > 0.0f ? 2.0f + scaleWidth : 0.0f);
        const float x = area.getCentreX() - groupWidth * 0.5f;
        l.column = { x, area.getY(), columnWidth, area.getHeight() };
        if (scaleWidth > 0.0f)
            l.scale = { x + columnWidth + 2.0f, area.getY(), scaleWidth, area.getHeight() };

        // Segments of about 4 px, so the column reads as LEDs at any height.
        l.segments = juce::jmax (0, (int) std::floor (l.column.getHeight() / 4.0f));
        return l;
    }

    float valueToY (float db, juce::Rectangle<float> column) const
    {
        const float proportion = (juce::jlimit (minDb, maxDb, db) - minDb) / (maxDb - minDb);
        return column.getBottom() - proportion * column.getHeight();
    }

    static juce::String formatDb (float db)
    {
        if (std::abs (db) < 0.05f)
            return "0.0";
        return (db > 0.0f ? "+" : "-") + juce::String (std::abs (db), 1);
    }

    static juce::String formatTick (float db)
    {
        const int v = juce::roundToInt (db);
        return v > 0 ? "+" + juce::String (v) : juce::String (v);
    }

    float minDb = -30.0f, maxDb = 0.0f;
    std::vector<float> ticks;
    float valueDb = 0.0f, peakDb = 0.0f, shownPeakDb = 0.0f;
    double lastUpdateMs = 0.0, peakSinceMs = 0.0;
    double peakHoldMs = 1500.0, fallDbPerSecond = 12.0;
    juce::Colour colour { juce::Colours::orange };
    bool active = true, showScale = true, showReadout = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GainReductionMeter)
};

} // namespace fxme
