/*
  ------------------------------------------------------------------------------
    DynamicsMeter.h

    The meter of one dynamics stage (a compressor, a dynamic EQ band): two
    vertical bars side by side.

      Level   the detector's level on a dBFS scale, with a peak marker held
              for a second, and the threshold drawn across it on the same
              scale, so where the level stands against the threshold reads at
              a glance. The threshold marker can be dragged (onThresholdDrag),
              which makes the meter a threshold control as well.
      Gain    the gain change the stage applies, in dB, growing from a 0 dB
              line in the middle: down for a cut, up for a boost, on a
              +/- range scale (setGainRange).

    Push model: call update() from the editor's timer with the latest values
    (read from the audio thread's atomics); the meter repaints itself. It
    owns no timer and reads nothing on its own.

    The level and the threshold must be on the same scale: the detector's
    band-filtered level, not a spectrum bin's, which is why this meter, and
    not a spectrum display, is where a threshold belongs.

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>

namespace fxme
{

class DynamicsMeter : public juce::Component,
                      public juce::SettableTooltipClient
{
public:
    struct Colours
    {
        juce::Colour background { 0xff0e0d14 };
        juce::Colour outline    { 0xff3a3a4c };
        juce::Colour level      { 0xff7f8cff };
        juce::Colour peak       { 0xffd8d8e0 };
        juce::Colour threshold  { 0xffffb03a };
        juce::Colour cut        { 0xffff6a5a };
        juce::Colour boost      { 0xff5ad68a };
        juce::Colour text       { 0xff9a9aa8 };
    };

    DynamicsMeter() = default;

    void setColours (Colours c)                 { colours = c; repaint(); }
    const Colours& getColours() const noexcept  { return colours; }

    /** The level scale, in dBFS (default -60 to 0). The threshold uses it too. */
    void setLevelRange (float minDbIn, float maxDbIn)
    {
        minDb = minDbIn;
        maxDb = juce::jmax (minDbIn + 1.0f, maxDbIn);
        repaint();
    }

    /** The gain bar's scale: +/- maxAbsDb (default 24). */
    void setGainRange (float maxAbsDb)
    {
        gainRange = juce::jmax (1.0f, maxAbsDb);
        repaint();
    }

    /** Labels under the two bars (default "Lvl" and "GR"). */
    void setLabels (juce::String levelLabel, juce::String gainLabel)
    {
        labelLevel = std::move (levelLabel);
        labelGain = std::move (gainLabel);
        repaint();
    }

    /** The latest values. `thresholdActive` false draws the threshold dimmed
        (a stage switched off, say). */
    void update (float levelDbIn, float thresholdDbIn, float gainChangeDbIn, bool thresholdActiveIn = true)
    {
        const auto now = juce::Time::getMillisecondCounter();
        if (levelDbIn >= peakDb || now - peakTime > peakHoldMs)
        {
            peakDb = levelDbIn;
            peakTime = now;
        }

        if (levelDbIn == levelDb && thresholdDbIn == thresholdDb && gainChangeDbIn == gainDb
            && thresholdActiveIn == thresholdActive)
            return;

        levelDb = levelDbIn;
        thresholdDb = thresholdDbIn;
        gainDb = gainChangeDbIn;
        thresholdActive = thresholdActiveIn;
        repaint();
    }

    /** Dragging the threshold marker: begin, each new value (dBFS), end. Left
        null, the marker cannot be dragged. */
    std::function<void()> onThresholdDragStart;
    std::function<void (float)> onThresholdDrag;
    std::function<void()> onThresholdDragEnd;

    //==========================================================================
    void paint (juce::Graphics& g) override
    {
        const auto lvl = levelBar(), gr = gainBar();
        const bool on = isEnabled();
        auto fade = [on] (juce::Colour c) { return on ? c : c.withMultipliedSaturation (0.3f).withMultipliedAlpha (0.5f); };

        for (auto r : { lvl, gr })
        {
            g.setColour (colours.background);
            g.fillRoundedRectangle (r, 2.0f);
        }

        // Level, from the bottom.
        const float yLevel = levelToY (levelDb, lvl);
        g.setColour (fade (colours.level));
        g.fillRect (lvl.withTop (yLevel));

        g.setColour (fade (colours.peak).withAlpha (0.8f));
        g.fillRect (lvl.getX(), levelToY (peakDb, lvl) - 1.0f, lvl.getWidth(), 1.5f);

        // Threshold across the level bar, with a notch on the left edge.
        {
            const float y = levelToY (thresholdDb, lvl);
            auto c = fade (colours.threshold).withMultipliedAlpha (thresholdActive ? 1.0f : 0.4f);
            g.setColour (c);
            g.fillRect (lvl.getX() - 3.0f, y - 1.0f, lvl.getWidth() + 3.0f, 2.0f);
            juce::Path tri;
            tri.addTriangle (lvl.getX() - 7.0f, y - 4.0f, lvl.getX() - 7.0f, y + 4.0f, lvl.getX() - 2.0f, y);
            g.fillPath (tri);
        }

        // Gain change, from the 0 dB line in the middle.
        {
            const float y0 = gainToY (0.0f, gr), y = gainToY (gainDb, gr);
            g.setColour (fade (gainDb < 0.0f ? colours.cut : colours.boost));
            g.fillRect (juce::Rectangle<float>::leftTopRightBottom (gr.getX(), juce::jmin (y0, y),
                                                                     gr.getRight(), juce::jmax (y0, y)));
            g.setColour (colours.outline);
            g.fillRect (gr.getX(), y0 - 0.5f, gr.getWidth(), 1.0f);
        }

        g.setColour (colours.outline);
        for (auto r : { lvl, gr })
            g.drawRoundedRectangle (r, 2.0f, 1.0f);

        // Scale ticks and read-outs.
        g.setFont (juce::FontOptions (10.0f));
        g.setColour (colours.text);
        for (float db = std::ceil (maxDb / 12.0f) * 12.0f; db >= minDb; db -= 12.0f)
        {
            const float y = levelToY (db, lvl);
            g.fillRect (lvl.getRight() + 1.0f, y, 3.0f, 1.0f);
        }

        auto labels = getLocalBounds().toFloat().removeFromBottom (labelHeight);
        g.drawText (labelLevel, labels.withX (lvl.getX() - 8.0f).withWidth (lvl.getWidth() + 16.0f),
                    juce::Justification::centred);
        g.drawText (labelGain, labels.withX (gr.getX() - 8.0f).withWidth (gr.getWidth() + 16.0f),
                    juce::Justification::centred);

        auto top = getLocalBounds().toFloat().removeFromTop (readoutHeight);
        g.setColour (colours.text.brighter (0.4f));
        g.drawText (levelDb <= minDb ? juce::String ("-inf") : juce::String (levelDb, 1),
                    top.withX (lvl.getX() - 10.0f).withWidth (lvl.getWidth() + 20.0f),
                    juce::Justification::centred);
        g.drawText ((gainDb > 0.05f ? "+" : "") + juce::String (gainDb, 1),
                    top.withX (gr.getX() - 10.0f).withWidth (gr.getWidth() + 20.0f),
                    juce::Justification::centred);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        draggingThreshold = onThresholdDrag != nullptr
                         && std::abs (e.position.y - levelToY (thresholdDb, levelBar())) < 6.0f
                         && e.position.x < levelBar().getRight() + 4.0f;
        if (draggingThreshold && onThresholdDragStart)
            onThresholdDragStart();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (draggingThreshold && onThresholdDrag)
            onThresholdDrag (yToLevel (e.position.y, levelBar()));
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (draggingThreshold && onThresholdDragEnd)
            onThresholdDragEnd();
        draggingThreshold = false;
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const bool over = onThresholdDrag != nullptr
                       && std::abs (e.position.y - levelToY (thresholdDb, levelBar())) < 6.0f;
        setMouseCursor (over ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
    }

private:
    static constexpr float labelHeight = 14.0f, readoutHeight = 14.0f;

    juce::Rectangle<float> bars() const
    {
        return getLocalBounds().toFloat().withTrimmedTop (readoutHeight + 2.0f)
                   .withTrimmedBottom (labelHeight + 2.0f).reduced (8.0f, 0.0f);
    }

    juce::Rectangle<float> levelBar() const
    {
        auto b = bars();
        const float w = juce::jmin (16.0f, (b.getWidth() - 12.0f) * 0.5f);
        return { b.getCentreX() - 6.0f - w, b.getY(), w, b.getHeight() };
    }

    juce::Rectangle<float> gainBar() const
    {
        auto l = levelBar();
        return l.withX (l.getRight() + 12.0f);
    }

    float levelToY (float db, juce::Rectangle<float> r) const
    {
        return juce::jmap (juce::jlimit (minDb, maxDb, db), minDb, maxDb, r.getBottom(), r.getY());
    }

    float yToLevel (float y, juce::Rectangle<float> r) const
    {
        return juce::jmap (juce::jlimit (r.getY(), r.getBottom(), y), r.getBottom(), r.getY(), minDb, maxDb);
    }

    float gainToY (float db, juce::Rectangle<float> r) const
    {
        return juce::jmap (juce::jlimit (-gainRange, gainRange, db), -gainRange, gainRange,
                           r.getBottom(), r.getY());
    }

    Colours colours;
    float minDb = -60.0f, maxDb = 0.0f, gainRange = 24.0f;
    float levelDb = -150.0f, thresholdDb = -20.0f, gainDb = 0.0f;
    bool thresholdActive = true;
    float peakDb = -150.0f;
    juce::uint32 peakTime = 0;
    static constexpr juce::uint32 peakHoldMs = 1000;
    bool draggingThreshold = false;
    juce::String labelLevel { "Lvl" }, labelGain { "Gain" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DynamicsMeter)
};

} // namespace fxme
