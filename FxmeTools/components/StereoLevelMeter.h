/*
  ------------------------------------------------------------------------------
    StereoLevelMeter.h

    A compact one- or two-channel vertical level meter: an RMS bar per
    channel, a peak marker held for a second, red above 0 dBFS, on a dBFS
    scale (-60 to +6 by default).

    Push model, like DynamicsMeter: the editor's timer calls update() with
    the latest values from the audio thread's atomics.

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

class StereoLevelMeter : public juce::Component,
                         public juce::SettableTooltipClient
{
public:
    struct Colours
    {
        juce::Colour background { 0xff0e0d14 };
        juce::Colour outline    { 0xff3a3a4c };
        juce::Colour bar        { 0xff35d6d0 };
        juce::Colour peak       { 0xffd8d8e0 };
        juce::Colour over       { 0xffff5040 };
    };

    StereoLevelMeter() = default;

    void setColours (Colours c)          { colours = c; repaint(); }
    void setNumChannels (int n)          { numChannels = juce::jlimit (1, 2, n); repaint(); }
    void setRange (float minDbIn, float maxDbIn)
    {
        minDb = minDbIn;
        maxDb = juce::jmax (minDbIn + 1.0f, maxDbIn);
        repaint();
    }

    /** Channel `ch` (0 or 1): its RMS and its peak since the last update, in dBFS. */
    void update (int ch, float rmsDb, float peakDbIn)
    {
        if (! juce::isPositiveAndBelow (ch, 2))
            return;
        auto& c = chans[(size_t) ch];
        const auto now = juce::Time::getMillisecondCounter();
        if (peakDbIn >= c.held || now - c.heldTime > 1000)
        {
            c.held = peakDbIn;
            c.heldTime = now;
        }
        c.rms = rmsDb;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (1.0f);
        const float gap = 2.0f;
        const float w = (b.getWidth() - gap * (float) (numChannels - 1)) / (float) numChannels;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto r = juce::Rectangle<float> (b.getX() + (float) ch * (w + gap), b.getY(), w, b.getHeight());
            const auto& c = chans[(size_t) ch];

            g.setColour (colours.background);
            g.fillRect (r);

            const float y0 = toY (0.0f, r);
            const float y = toY (c.rms, r);
            g.setColour (colours.bar);
            g.fillRect (r.withTop (juce::jmax (y, y0)));
            if (y < y0)
            {
                g.setColour (colours.over);
                g.fillRect (r.withTop (y).withBottom (y0));
            }

            g.setColour (c.held > 0.0f ? colours.over : colours.peak);
            g.fillRect (r.getX(), toY (c.held, r) - 1.0f, r.getWidth(), 1.5f);

            g.setColour (colours.outline);
            g.drawRect (r, 1.0f);
            g.fillRect (r.getX(), y0, r.getWidth(), 1.0f);
        }
    }

private:
    float toY (float db, juce::Rectangle<float> r) const
    {
        return juce::jmap (juce::jlimit (minDb, maxDb, db), minDb, maxDb, r.getBottom(), r.getY());
    }

    struct Channel
    {
        float rms = -150.0f, held = -150.0f;
        juce::uint32 heldTime = 0;
    };

    Colours colours;
    int numChannels = 2;
    float minDb = -60.0f, maxDb = 6.0f;
    std::array<Channel, 2> chans;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StereoLevelMeter)
};

} // namespace fxme
