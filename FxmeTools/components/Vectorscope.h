/*
  ------------------------------------------------------------------------------
    Vectorscope.h

    Stereo vectorscope (Lissajous / goniometer) over an fxme::StereoTap:
    mid (L + R) up, side (R - L) across, so a mono signal is a vertical line,
    wide stereo a cloud, and out-of-phase content leans towards the
    horizontal. The trace leaves a fading trail (the previous frames are
    dimmed into an image rather than redrawn), which is cheap at 30 to 60 fps
    and reads like a phosphor screen.

    Usage (editor):
        fxme::Vectorscope scope { processor.getOutputTap() };
        scope.setColours ({ accent, grid, background });
        addAndMakeVisible (scope);

    The component polls the tap from its own timer (default 30 fps) and
    repaints only when new frames arrived. Message thread only.

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>
#include <FxmeTools/dsp/StereoTap.h>

namespace fxme
{

class Vectorscope : public juce::Component,
                    public juce::SettableTooltipClient,
                    private juce::Timer
{
public:
    struct Colours
    {
        juce::Colour trace      { 0xffff8a1f };
        juce::Colour grid       { 0x40ffffff };
        juce::Colour background { 0xff101014 };
    };

    explicit Vectorscope (const StereoTap& tapToShow, int framesPerRefresh = 1024)
        : tap (tapToShow), frames (framesPerRefresh)
    {
        bufL.resize ((size_t) frames);
        bufR.resize ((size_t) frames);
        setOpaque (false);
        startTimerHz (30);
    }

    void setColours (Colours c) { colours = c; trail = {}; repaint(); }
    void setRefreshRate (int hz) { startTimerHz (juce::jlimit (5, 60, hz)); }

    /** How much of the previous trail is kept each refresh (0 .. 1). */
    void setPersistence (float p) { persistence = juce::jlimit (0.0f, 0.98f, p); }

    /** Amplitude shown at the edge of the circle (1 = full scale). */
    void setScale (float fullScale) { scale = juce::jmax (0.01f, fullScale); }

    /** Draws the frame (circle and axes) or just the trace. */
    void setDrawGrid (bool shouldDraw) { drawGrid = shouldDraw; repaint(); }

    void resized() override { trail = {}; }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        const float d = juce::jmin (b.getWidth(), b.getHeight());
        const auto area = juce::Rectangle<float> (d, d).withCentre (b.getCentre());

        g.setColour (colours.background);
        g.fillEllipse (area);

        if (trail.isValid())
        {
            juce::Graphics::ScopedSaveState s (g);
            juce::Path clip;
            clip.addEllipse (area);
            g.reduceClipRegion (clip);
            g.drawImageAt (trail, (int) area.getX(), (int) area.getY());
        }

        if (drawGrid)
        {
            g.setColour (colours.grid);
            g.drawEllipse (area.reduced (0.5f), 1.0f);
            const auto c = area.getCentre();
            const float r = d * 0.5f;
            g.drawLine (c.x, c.y - r, c.x, c.y + r, 0.6f);                     // mid
            g.drawLine (c.x - r, c.y, c.x + r, c.y, 0.6f);                     // side
            const float k = r * 0.7071f;
            g.drawLine (c.x - k, c.y - k, c.x + k, c.y + k, 0.4f);             // L
            g.drawLine (c.x + k, c.y - k, c.x - k, c.y + k, 0.4f);             // R
        }
    }

private:
    void timerCallback() override
    {
        const auto total = tap.getTotalPushed();
        if (total == lastTotal && ! fading)
            return;
        const bool fresh = total != lastTotal;
        lastTotal = total;

        const int d = juce::jmin (getWidth(), getHeight());
        if (d <= 2)
            return;

        if (! trail.isValid() || trail.getWidth() != d)
            trail = juce::Image (juce::Image::ARGB, d, d, true);

        // Dim what was there: multiply alpha by the persistence.
        trail.multiplyAllAlphas (persistence);
        fading = true;

        if (fresh)
        {
            juce::Graphics g (trail);
            tap.snapshot (bufL.data(), bufR.data(), frames);
            const float c = (float) d * 0.5f;
            const float k = c / scale * 0.7071f;
            g.setColour (colours.trace.withAlpha (0.55f));
            for (int i = 0; i < frames; ++i)
            {
                const float l = bufL[(size_t) i], r = bufR[(size_t) i];
                const float x = c + (r - l) * k;
                const float y = c - (l + r) * k;
                g.fillRect (x - 0.6f, y - 0.6f, 1.2f, 1.2f);
            }
        }
        else if (++idleFrames > 60)
        {
            fading = false;   // the trail has faded out: stop repainting
            idleFrames = 0;
        }

        if (fresh)
            idleFrames = 0;

        repaint();
    }

    const StereoTap& tap;
    int frames;
    std::vector<float> bufL, bufR;
    juce::Image trail;
    Colours colours;
    float persistence = 0.7f, scale = 1.0f;
    bool drawGrid = true, fading = false;
    int idleFrames = 0;
    std::int64_t lastTotal = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Vectorscope)
};

} // namespace fxme
