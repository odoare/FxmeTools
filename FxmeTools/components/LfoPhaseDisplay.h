/*
  ------------------------------------------------------------------------------
    LfoPhaseDisplay.h

    Where a modulation effect's LFO stands: one cycle of its actual waveform
    (fxme::Lfo's shapes) with a dot per reader moving along it. A chorus,
    flanger or phaser reads one fxme::ModLfo at several phase offsets (the
    right channel half a cycle ahead at full width, extra voices spread
    around the cycle): each offset is a dot, so the shape, the current phase
    and the stereo spread show at once.

    Push model: the editor's timer calls setPhase() with the LFO's master
    phase (in cycles, read from an atomic the audio thread updates) and
    setShape() / setDots() when the parameters change. It repaints itself
    when what it shows changes; it owns no timer.

    setActive (false) greys it (the effect is off). Pure display; depends on
    JUCE and on FxmeTools' core Lfo for the shapes.

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>
#include <FxmeTools/dsp/Lfo.h>

#include <cmath>
#include <vector>

namespace fxme
{

class LfoPhaseDisplay : public juce::Component,
                        public juce::SettableTooltipClient
{
public:
    /** One reader of the LFO: its offset from the master phase, in cycles,
        and a short label drawn by its dot ("L", "R"; empty for none). A
        secondary dot is drawn smaller (an extra voice). */
    struct Dot
    {
        float offset = 0.0f;
        juce::String label;
        bool secondary = false;
    };

    LfoPhaseDisplay()
    {
        dots = { { 0.0f, "L", false }, { 0.5f, "R", false } };
    }

    /** fxme::Lfo shape index. */
    void setShape (int newShape)                { if (newShape != shape) { shape = newShape; repaint(); } }

    /** The master phase, in cycles (any value: it is wrapped). */
    void setPhase (float newPhase)
    {
        newPhase = wrap (newPhase);
        if (std::abs (newPhase - phase) > 1.0e-4f)
        {
            phase = newPhase;
            repaint();
        }
    }

    void setDots (std::vector<Dot> newDots)
    {
        bool same = newDots.size() == dots.size();
        for (std::size_t i = 0; same && i < dots.size(); ++i)
            same = std::abs (newDots[i].offset - dots[i].offset) < 1.0e-4f && newDots[i].label == dots[i].label
                && newDots[i].secondary == dots[i].secondary;
        if (! same)
        {
            dots = std::move (newDots);
            repaint();
        }
    }

    /** A short text in the corner (the rate, say). */
    void setCaption (const juce::String& text)  { if (text != caption) { caption = text; repaint(); } }

    void setAccentColour (juce::Colour c)       { if (c != colour) { colour = c; repaint(); } }

    /** False greys the display (the effect is off). */
    void setActive (bool shouldBeActive)        { if (shouldBeActive != active) { active = shouldBeActive; repaint(); } }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced (1.0f);
        const auto accent = active ? colour : colour.withSaturation (colour.getSaturation() * 0.3f).withMultipliedAlpha (0.5f);
        const auto text = juce::Colours::white.withAlpha (active ? 0.7f : 0.3f);

        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.fillRoundedRectangle (bounds, 4.0f);
        g.setColour (accent.withMultipliedAlpha (0.35f));
        g.drawRoundedRectangle (bounds, 4.0f, 1.0f);

        // The curve's area: room for the dots' radius and their labels.
        const float dotRadius = juce::jlimit (2.5f, 5.0f, bounds.getHeight() * 0.07f);
        const auto plot = bounds.reduced (dotRadius + 6.0f, dotRadius + 4.0f);
        if (plot.getWidth() < 8.0f || plot.getHeight() < 6.0f)
            return;

        // The centre line, and one cycle of the waveform.
        g.setColour (juce::Colours::white.withAlpha (0.12f));
        g.drawHorizontalLine (juce::roundToInt (plot.getCentreY()), plot.getX(), plot.getRight());

        juce::Path curve;
        const int steps = juce::jmax (16, (int) plot.getWidth());
        for (int i = 0; i <= steps; ++i)
        {
            const float p = (float) i / (float) steps;
            const auto point = pointAt (p, plot);
            if (i == 0)
                curve.startNewSubPath (point);
            else
                curve.lineTo (point);
        }
        g.setColour (accent.withMultipliedAlpha (0.6f));
        g.strokePath (curve, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // The readers: secondary dots first, so the main ones stay on top.
        const float labelSize = juce::jlimit (8.0f, 11.0f, bounds.getHeight() * 0.16f);
        g.setFont (juce::Font (juce::FontOptions (labelSize, juce::Font::bold)));
        for (const bool drawingSecondary : { true, false })
            for (const auto& d : dots)
            {
                if (d.secondary != drawingSecondary)
                    continue;
                const float p = wrap (phase + d.offset);
                const auto point = pointAt (p, plot);
                const float r = d.secondary ? dotRadius * 0.65f : dotRadius;

                g.setColour (accent.withMultipliedAlpha (0.25f));
                g.fillEllipse (juce::Rectangle<float> (r * 3.2f, r * 3.2f).withCentre (point));
                g.setColour (d.secondary ? accent : accent.brighter (0.5f));
                g.fillEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (point));

                if (d.label.isNotEmpty())
                {
                    // Above the dot in the lower half, below it in the upper one.
                    const bool above = point.y > plot.getCentreY();
                    const auto box = juce::Rectangle<float> (labelSize * 2.0f, labelSize * 1.2f)
                                         .withCentre ({ point.x, point.y + (above ? -1.0f : 1.0f) * (r + labelSize * 0.7f) });
                    g.setColour (text);
                    g.drawText (d.label, box, juce::Justification::centred, false);
                }
            }

        if (caption.isNotEmpty() && bounds.getHeight() > 30.0f)
        {
            g.setColour (text.withMultipliedAlpha (0.8f));
            g.setFont (juce::Font (juce::FontOptions (juce::jlimit (8.0f, 10.5f, bounds.getHeight() * 0.15f))));
            g.drawText (caption, bounds.reduced (5.0f, 2.0f), juce::Justification::bottomRight, false);
        }
    }

private:
    juce::Point<float> pointAt (float p, juce::Rectangle<float> plot) const
    {
        const float v = juce::jlimit (-1.0f, 1.0f, Lfo::eval (shape, p));
        return { plot.getX() + p * plot.getWidth(), plot.getCentreY() - v * plot.getHeight() * 0.5f };
    }

    static float wrap (float p) noexcept
    {
        p -= std::floor (p);
        return p >= 1.0f ? 0.0f : p;
    }

    int shape = 0;
    float phase = 0.0f;
    std::vector<Dot> dots;
    juce::String caption;
    juce::Colour colour { juce::Colours::cyan };
    bool active = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LfoPhaseDisplay)
};

} // namespace fxme
