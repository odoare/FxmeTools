/*
  ------------------------------------------------------------------------------
    WaveformDisplay.h

    Reusable time-domain signal view, the temporal sibling of SpectrumDisplay:
    draws one audio signal (all its channels overlaid) on a time/amplitude
    grid styled like the spectrum plots, with a marker/region system and
    mouse-driven navigation.

    Sources (exclusive — setting one replaces the previous):
      * setBuffer()  — a static AudioBuffer copy (an impulse response, an
                       analysis result, ...). Draws fast at any zoom thanks to
                       a min/max peak cache, so signals of dozens of seconds
                       are fine.
      * setFile()    — a wav/aiff/flac file, loaded through the same path.
      * setTap()     — realtime monitoring of a WaveformTap the audio thread
                       pushes into; the view scrolls, always ending "now"
                       (time axis is negative seconds into the past).

    Markers are labelled vertical time lines, regions are labelled shaded
    time spans; both are pushed as complete sets by the owner (setMarkers /
    setRegions) in display time.

    Display time vs data time: sample 0 of the buffer sits at
    t = -getTimeOffset(). Use setTimeOffset() to re-origin the axis, e.g. put
    t = 0 on the acausal centre of a linear-phase impulse response.

    Mouse:
      * wheel            vertical (amplitude) zoom
      * ctrl + wheel     horizontal (time) zoom around the cursor
      * click-drag       pan through the data (static sources)
      * double-click     reset to the full view (fit amplitude to the data)
    A read-out in the top-right corner shows time + amplitude at the cursor.
    When channel names are set, the legend is clickable: a click on an entry
    hides/shows that channel's trace.

    Palette is injected via setColours() (defaults to the same dark theme as
    SpectrumDisplay); per-channel trace colours via setChannelColours() and
    an optional legend via setChannelNames().

    Message thread only (the tap's push() side excepted).

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>
#include <FxmeTools/dsp/WaveformTap.h>
#include <cmath>
#include <vector>

namespace fxme
{

class WaveformDisplay : public juce::Component,
                        private juce::Timer
{
public:
    /** Palette for the grid, labels and chrome (same fields and defaults as
        SpectrumDisplay::Colours). Trace colours are per channel, see
        setChannelColours(). */
    struct Colours
    {
        juce::Colour plotBackground { 0xff000000 };
        juce::Colour grid           { 0x66555555 };
        juce::Colour gridZero       { 0xcc555555 };   // t = 0 / zero-amplitude line
        juce::Colour text           { 0xffd8d8e0 };
        juce::Colour dimText        { 0xff9a9aa8 };
        juce::Colour panelLine      { 0xff3a3a4c };
    };

    /** A labelled vertical line at a display time. */
    struct Marker
    {
        double timeS = 0.0;
        juce::String label;
        juce::Colour colour { 0xffd8d8e0 };
    };

    /** A labelled shaded time span. */
    struct Region
    {
        double startS = 0.0, endS = 0.0;
        juce::String label;
        juce::Colour colour { 0x33d8d8e0 };     // drawn as-is: bake the alpha in
    };

    WaveformDisplay() = default;

    //==========================================================================
    // Sources

    /** Copies the buffer and shows it in full (amplitude fitted). Pass
        resetViewToFull = false to keep the current zoom/pan — useful when the
        owner re-renders the same signal after a parameter tweak; the window
        is still re-clamped to the new data. */
    void setBuffer (const juce::AudioBuffer<float>& data, double newSampleRate,
                    bool resetViewToFull = true)
    {
        tap = nullptr;
        stopTimer();
        audio.makeCopyOf (data);
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        rebuildCache();
        if (resetViewToFull)
            resetView();
        else
            setTimeWindow (viewStartS, viewLengthS);
    }

    /** Loads an audio file (wav/aiff/flac...) into the buffer source. Files
        longer than maxFileSeconds are truncated. Returns false (and keeps the
        previous contents) when the file cannot be read. */
    bool setFile (const juce::File& file)
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
        if (reader == nullptr || reader->numChannels == 0 || reader->lengthInSamples <= 0)
            return false;

        const auto len = juce::jmin (reader->lengthInSamples,
                                     (juce::int64) (maxFileSeconds * reader->sampleRate));
        juce::AudioBuffer<float> b ((int) reader->numChannels, (int) len);
        reader->read (&b, 0, (int) len, 0, true, true);
        setBuffer (b, reader->sampleRate);
        return true;
    }

    /** Realtime monitoring of a tap (single channel). The caller keeps
        ownership; pass nullptr to detach. The view always ends "now" — the
        time axis shows negative seconds into the past — so horizontal panning
        is disabled and ctrl-wheel adjusts the visible history length. */
    void setTap (WaveformTap* newTap)
    {
        tap = newTap;
        audio.setSize (0, 0);
        cache.clear();
        if (tap != nullptr)
        {
            sampleRate = tap->getSampleRate() > 0.0 ? tap->getSampleRate() : 44100.0;
            viewLengthS = (double) tap->getCapacity() / sampleRate;
            ampRange = 1.0f;
            startTimerHz (25);
        }
        else
        {
            stopTimer();
        }
        repaint();
    }

    void clear()
    {
        tap = nullptr;
        stopTimer();
        audio.setSize (0, 0);
        cache.clear();
        markers.clear();
        regions.clear();
        repaint();
    }

    bool hasData() const noexcept
    {
        return tap != nullptr || audio.getNumSamples() > 0;
    }

    double getSampleRate() const noexcept   { return sampleRate; }

    //==========================================================================
    // Appearance

    void setColours (Colours c)                         { colours = c; repaint(); }
    const Colours& getColours() const noexcept          { return colours; }

    /** One colour per channel (cycled when the source has more channels). */
    void setChannelColours (std::vector<juce::Colour> c) { channelColours = std::move (c); repaint(); }

    /** Legend labels, one per channel; empty = no legend. */
    void setChannelNames (juce::StringArray names)      { channelNames = std::move (names); repaint(); }

    //==========================================================================
    // Markers & regions (display time, i.e. the axis the user sees)

    void setMarkers (std::vector<Marker> m)             { markers = std::move (m); repaint(); }
    void setRegions (std::vector<Region> r)             { regions = std::move (r); repaint(); }

    //==========================================================================
    // View

    /** Shifts the time axis: buffer sample 0 displays at t = -seconds. E.g.
        pass firLength/2 / sampleRate to put t = 0 on the centre of a
        linear-phase IR. Buffer sources only; call before or after setBuffer
        (the full view is recomputed). */
    void setTimeOffset (double seconds)
    {
        timeOffsetS = seconds;
        if (tap == nullptr && audio.getNumSamples() > 0)
            resetView();
        else
            repaint();
    }

    double getTimeOffset() const noexcept               { return timeOffsetS; }

    /** Explicit time window (display time), clamped to the data. */
    void setTimeWindow (double startS, double lengthS)
    {
        viewLengthS = clampViewLength (lengthS);
        viewStartS  = juce::jlimit (fullStartS(), fullStartS() + fullLengthS() - viewLengthS, startS);
        repaint();
    }

    /** Vertical range: the plot spans ±maxAbs. */
    void setAmplitudeRange (float maxAbs)
    {
        ampRange = juce::jlimit (minAmp, maxAmp, maxAbs);
        repaint();
    }

    /** Full-signal view; buffer sources also fit the amplitude to the data
        peak. Same as a double-click. */
    void resetView()
    {
        viewStartS = fullStartS();
        viewLengthS = fullLengthS();
        if (tap == nullptr)
            ampRange = juce::jlimit (minAmp, maxAmp, dataPeak * 1.05f);
        repaint();
    }

    //==========================================================================
    // Mouse

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        const auto plot = getPlotArea();
        if (! plot.contains (e.position))
            return;

        const float factor = w.deltaY > 0.0f ? 0.85f : 1.0f / 0.85f;

        if (e.mods.isCtrlDown() || e.mods.isCommandDown())
        {
            // Horizontal zoom around the cursor's time.
            const double tAtX = xToTime (e.position.x, plot);
            const double newLen = clampViewLength (viewLengthS * (double) factor);
            if (tap != nullptr)
            {
                viewLengthS = newLen;   // the window stays glued to "now"
            }
            else
            {
                const double frac = viewLengthS > 0.0 ? (tAtX - viewStartS) / viewLengthS : 0.5;
                setTimeWindow (tAtX - frac * newLen, newLen);
                return;
            }
        }
        else
        {
            // Vertical (amplitude) zoom, symmetric around zero.
            ampRange = juce::jlimit (minAmp, maxAmp, ampRange * factor);
        }
        repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        // A click on a legend entry shows/hides that channel.
        for (const auto& hit : legendHits)
            if (hit.first.contains (e.getPosition()))
            {
                channelHidden.resize ((size_t) juce::jmax ((int) channelHidden.size(),
                                                           hit.second + 1), false);
                channelHidden[(size_t) hit.second] = ! channelHidden[(size_t) hit.second];
                dragging = false;
                repaint();
                return;
            }

        dragging = tap == nullptr && getPlotArea().contains (e.position);
        dragStartX = e.position.x;
        dragStartViewS = viewStartS;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging)
            return;
        const auto plot = getPlotArea();
        const double sPerPx = viewLengthS / juce::jmax (1.0, (double) plot.getWidth());
        setTimeWindow (dragStartViewS - (e.position.x - dragStartX) * sPerPx, viewLengthS);
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (getPlotArea().contains (e.position))
            resetView();
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        cursorPos = e.position;
        const bool in = getPlotArea().contains (e.position);
        if (in != cursorInPlot || in)
        {
            cursorInPlot = in;
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (cursorInPlot) { cursorInPlot = false; repaint(); }
    }

    //==========================================================================
    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        g.setColour (colours.plotBackground);
        g.fillRoundedRectangle (bounds, 4.0f);

        const auto plot = getPlotArea();
        drawGrid (g, plot);
        drawRegions (g, plot);

        {
            juce::Graphics::ScopedSaveState clip (g);
            g.reduceClipRegion (plot.toNearestInt());
            if (tap != nullptr)
                drawTapTrace (g, plot);
            else
                drawBufferTraces (g, plot);
        }

        drawMarkers (g, plot);
        drawLegend (g, plot);
        if (cursorInPlot)
            drawCursorReadout (g, plot);

        g.setColour (colours.panelLine);
        g.drawRoundedRectangle (bounds.reduced (0.5f), 4.0f, 1.0f);
    }

private:
    static constexpr int cacheBucket = 256;         // samples per min/max bucket
    static constexpr double maxFileSeconds = 600.0; // setFile truncation
    static constexpr float minAmp = 1.0e-4f, maxAmp = 100.0f;

    //==========================================================================
    // Geometry & mapping

    juce::Rectangle<float> getPlotArea() const
    {
        return getLocalBounds().toFloat().reduced (8.0f)
                   .withTrimmedLeft (34.0f).withTrimmedBottom (14.0f);
    }

    double fullStartS() const
    {
        return tap != nullptr ? -(double) tap->getCapacity() / sampleRate
                              : -timeOffsetS;
    }

    double fullLengthS() const
    {
        const double n = tap != nullptr ? (double) tap->getCapacity()
                                        : (double) audio.getNumSamples();
        return juce::jmax (1.0e-3, n / sampleRate);
    }

    double minViewLengthS() const
    {
        return juce::jmax (1.0e-4, 32.0 / sampleRate);
    }

    // Clamp a requested window length to [minView, full], degrading
    // gracefully when the whole signal is shorter than the minimum zoom.
    double clampViewLength (double lengthS) const
    {
        const double full = fullLengthS();
        return juce::jlimit (juce::jmin (minViewLengthS(), full), full, lengthS);
    }

    // Tap mode: the window always ends "now" (t = 0).
    double effectiveViewStartS() const
    {
        return tap != nullptr ? -viewLengthS : viewStartS;
    }

    float timeToX (double t, juce::Rectangle<float> r) const
    {
        return r.getX() + (float) ((t - effectiveViewStartS()) / viewLengthS) * r.getWidth();
    }

    double xToTime (float x, juce::Rectangle<float> r) const
    {
        return effectiveViewStartS() + (double) ((x - r.getX()) / r.getWidth()) * viewLengthS;
    }

    float ampToY (float v, juce::Rectangle<float> r) const
    {
        return juce::jmap (juce::jlimit (-ampRange, ampRange, v),
                           -ampRange, ampRange, r.getBottom(), r.getY());
    }

    //==========================================================================
    // Grid & labels

    static double niceStep (double range, int targetDivs)
    {
        const double raw = juce::jmax (1.0e-12, range / (double) targetDivs);
        const double mag = std::pow (10.0, std::floor (std::log10 (raw)));
        const double n = raw / mag;
        return (n <= 1.0 ? 1.0 : n <= 2.0 ? 2.0 : n <= 5.0 ? 5.0 : 10.0) * mag;
    }

    // Time labels switch to milliseconds below half a second of window.
    juce::String timeLabel (double t, double step) const
    {
        const bool ms = viewLengthS < 0.5;
        const double v = ms ? t * 1000.0 : t;
        const double s = ms ? step * 1000.0 : step;
        const int decimals = juce::jmax (0, (int) std::ceil (-std::log10 (s) - 1.0e-9));
        return juce::String (v, decimals) + (ms ? " ms" : " s");
    }

    void drawGrid (juce::Graphics& g, juce::Rectangle<float> r) const
    {
        g.setFont (11.0f);

        // Vertical time lines at nice steps; t = 0 highlighted.
        const double t0 = effectiveViewStartS();
        const double step = niceStep (viewLengthS, 8);
        for (double t = std::ceil (t0 / step) * step; t <= t0 + viewLengthS; t += step)
        {
            const float x = timeToX (t, r);
            g.setColour (std::abs (t) < step * 0.5 ? colours.gridZero : colours.grid);
            g.drawVerticalLine ((int) x, r.getY(), r.getBottom());
            g.setColour (colours.dimText);
            g.drawText (timeLabel (t, step), (int) x - 34, (int) r.getBottom() + 1, 68, 12,
                        juce::Justification::centred);
        }

        // Horizontal amplitude lines; zero highlighted.
        const double aStep = niceStep (2.0 * (double) ampRange, 6);
        for (double a = -std::floor ((double) ampRange / aStep) * aStep;
             a <= (double) ampRange + 1.0e-9; a += aStep)
        {
            const float y = ampToY ((float) a, r);
            g.setColour (std::abs (a) < aStep * 0.5 ? colours.gridZero : colours.grid);
            g.drawHorizontalLine ((int) y, r.getX(), r.getRight());
            g.setColour (colours.dimText);
            const int decimals = juce::jmax (0, (int) std::ceil (-std::log10 (aStep) - 1.0e-9));
            g.drawText (juce::String (a, decimals), 2, (int) y - 6, 30, 12,
                        juce::Justification::centredRight);
        }
    }

    //==========================================================================
    // Waveform drawing

    // Draws one channel given raw samples and the display time of sample 0.
    // With a cache (buffer sources) zoomed-out columns aggregate buckets
    // instead of scanning every sample.
    void drawChannel (juce::Graphics& g, juce::Rectangle<float> r,
                      const float* data, int len, double tFirst,
                      const std::vector<std::pair<float, float>>* minMax,
                      juce::Colour colour) const
    {
        if (data == nullptr || len <= 0)
            return;

        const double sppx = viewLengthS * sampleRate / juce::jmax (1.0, (double) r.getWidth());
        g.setColour (colour);

        if (sppx <= 2.0)
        {
            // Zoomed in: connected polyline through the samples.
            juce::Path p;
            bool started = false;
            const int i0 = juce::jlimit (0, len - 1,
                (int) std::floor ((effectiveViewStartS() - tFirst) * sampleRate) - 1);
            const int i1 = juce::jlimit (0, len - 1,
                (int) std::ceil ((effectiveViewStartS() + viewLengthS - tFirst) * sampleRate) + 1);
            for (int i = i0; i <= i1; ++i)
            {
                const float x = timeToX (tFirst + (double) i / sampleRate, r);
                const float y = ampToY (data[i], r);
                if (! started) { p.startNewSubPath (x, y); started = true; }
                else             p.lineTo (x, y);
            }
            g.strokePath (p, juce::PathStrokeType (1.2f));
            return;
        }

        // Zoomed out: one min/max column per pixel.
        const int w = (int) r.getWidth();
        for (int px = 0; px < w; ++px)
        {
            const double tA = effectiveViewStartS() + viewLengthS * (double) px / (double) w;
            int s0 = (int) std::floor ((tA - tFirst) * sampleRate);
            int s1 = (int) std::floor ((tA + viewLengthS / (double) w - tFirst) * sampleRate) + 1;
            if (s1 <= 0)
                continue;       // column entirely before the data
            if (s0 >= len)
                break;          // past the data
            s0 = juce::jmax (0, s0);
            s1 = juce::jmin (len, s1);
            if (s1 <= s0)
                s1 = s0 + 1;    // s0 < len here, so s1 stays in range

            float lo, hi;
            if (minMax != nullptr && s1 - s0 >= 2 * cacheBucket)
            {
                const int b0 = s0 / cacheBucket;
                const int b1 = juce::jmin ((int) minMax->size() - 1, (s1 - 1) / cacheBucket);
                lo = (*minMax)[(size_t) b0].first;
                hi = (*minMax)[(size_t) b0].second;
                for (int b = b0 + 1; b <= b1; ++b)
                {
                    lo = std::min (lo, (*minMax)[(size_t) b].first);
                    hi = std::max (hi, (*minMax)[(size_t) b].second);
                }
            }
            else
            {
                lo = hi = data[s0];
                for (int i = s0 + 1; i < s1; ++i)
                {
                    lo = std::min (lo, data[i]);
                    hi = std::max (hi, data[i]);
                }
            }

            const float x = r.getX() + (float) px;
            const float yTop = ampToY (hi, r);
            const float yBot = ampToY (lo, r);
            g.drawVerticalLine ((int) x, yTop, juce::jmax (yBot, yTop + 1.0f));
        }
    }

    juce::Colour channelColour (int ch) const
    {
        if (! channelColours.empty())
            return channelColours[(size_t) (ch % (int) channelColours.size())];
        static const juce::Colour defaults[] = { juce::Colour (0xff62d0a8),
                                                 juce::Colour (0xffd9b13a),
                                                 juce::Colour (0xff6aa6e8),
                                                 juce::Colour (0xffd97070) };
        return defaults[ch % 4];
    }

    bool channelVisible (int ch) const
    {
        return ch >= (int) channelHidden.size() || ! channelHidden[(size_t) ch];
    }

    void drawBufferTraces (juce::Graphics& g, juce::Rectangle<float> r) const
    {
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            if (channelVisible (ch))
                drawChannel (g, r, audio.getReadPointer (ch), audio.getNumSamples(),
                             -timeOffsetS, ch < (int) cache.size() ? &cache[(size_t) ch] : nullptr,
                             channelColour (ch));
    }

    void drawTapTrace (juce::Graphics& g, juce::Rectangle<float> r)
    {
        if (! channelVisible (0))
            return;

        const int want = juce::jmin (tap->getCapacity(),
                                     (int) std::ceil (viewLengthS * sampleRate) + 1);
        tapScratch.resize ((size_t) juce::jmax (1, want));
        tap->snapshot (tapScratch.data(), want);
        // The newest snapshot sample sits at t = 0.
        drawChannel (g, r, tapScratch.data(), want,
                     -(double) (want - 1) / sampleRate, nullptr, channelColour (0));
    }

    //==========================================================================
    // Markers, regions, legend, cursor

    void drawRegions (juce::Graphics& g, juce::Rectangle<float> r) const
    {
        g.setFont (10.0f);
        for (const auto& reg : regions)
        {
            const float x0 = juce::jlimit (r.getX(), r.getRight(), timeToX (reg.startS, r));
            const float x1 = juce::jlimit (r.getX(), r.getRight(), timeToX (reg.endS, r));
            if (x1 <= x0)
                continue;
            g.setColour (reg.colour);
            g.fillRect (x0, r.getY(), x1 - x0, r.getHeight());
            if (reg.label.isNotEmpty())
            {
                g.setColour (colours.dimText);
                g.drawText (reg.label, (int) x0 + 3, (int) r.getY() + 2,
                            (int) (x1 - x0) - 6, 12, juce::Justification::centredLeft);
            }
        }
    }

    void drawMarkers (juce::Graphics& g, juce::Rectangle<float> r) const
    {
        g.setFont (10.0f);
        for (const auto& m : markers)
        {
            const float x = timeToX (m.timeS, r);
            if (x < r.getX() || x > r.getRight())
                continue;
            g.setColour (m.colour);
            g.drawVerticalLine ((int) x, r.getY(), r.getBottom());
            if (m.label.isNotEmpty())
                g.drawText (m.label, (int) x + 3, (int) r.getY() + 2, 80, 12,
                            juce::Justification::centredLeft);
        }
    }

    // One clickable entry per named channel (mouseDown scans legendHits);
    // a hidden channel's swatch and label are dimmed.
    void drawLegend (juce::Graphics& g, juce::Rectangle<float> r) const
    {
        legendHits.clear();
        if (channelNames.isEmpty())
            return;
        g.setFont (11.0f);
        int x = (int) r.getX() + 6;
        for (int ch = 0; ch < channelNames.size(); ++ch)
        {
            const bool on = channelVisible (ch);
            const auto& label = channelNames[ch];
            const int w = (int) juce::GlyphArrangement::getStringWidth (juce::Font (juce::FontOptions (11.0f)), label) + 6;
            g.setColour (on ? channelColour (ch) : channelColour (ch).withAlpha (0.3f));
            g.fillRect (x, (int) r.getBottom() - 10, 10, 3);
            g.setColour (on ? colours.text : colours.dimText.withAlpha (0.6f));
            g.drawText (label, x + 13, (int) r.getBottom() - 16, w, 14,
                        juce::Justification::centredLeft);
            legendHits.push_back ({ { x - 3, (int) r.getBottom() - 16, w + 18, 14 }, ch });
            x += w + 26;
        }
    }

    void drawCursorReadout (juce::Graphics& g, juce::Rectangle<float> r) const
    {
        const double t = xToTime (juce::jlimit (r.getX(), r.getRight(), cursorPos.x), r);
        const float cy = juce::jlimit (r.getY(), r.getBottom(), cursorPos.y);
        const float a = juce::jmap (cy, r.getBottom(), r.getY(), -ampRange, ampRange);

        const juce::String txt = timeLabel (t, viewLengthS / 1000.0) + "   " + juce::String (a, 3);

        g.setFont (11.0f);
        const int tw = (int) juce::GlyphArrangement::getStringWidth (juce::Font (juce::FontOptions (11.0f)), txt) + 12;
        juce::Rectangle<int> box ((int) r.getRight() - tw, (int) r.getY() + 2, tw, 15);
        g.setColour (colours.plotBackground.withAlpha (0.8f));
        g.fillRoundedRectangle (box.toFloat(), 3.0f);
        g.setColour (colours.panelLine);
        g.drawRoundedRectangle (box.toFloat(), 3.0f, 1.0f);
        g.setColour (colours.text);
        g.drawText (txt, box, juce::Justification::centred);
    }

    //==========================================================================
    void rebuildCache()
    {
        cache.clear();
        const int len = audio.getNumSamples();
        dataPeak = audio.getNumChannels() > 0 && len > 0
                       ? audio.getMagnitude (0, len) : 1.0f;
        if (dataPeak <= 0.0f)
            dataPeak = 1.0f;

        const int numBuckets = (len + cacheBucket - 1) / cacheBucket;
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        {
            std::vector<std::pair<float, float>> mm ((size_t) numBuckets);
            const float* d = audio.getReadPointer (ch);
            for (int b = 0; b < numBuckets; ++b)
            {
                const int s0 = b * cacheBucket;
                const int s1 = juce::jmin (len, s0 + cacheBucket);
                float lo = d[s0], hi = d[s0];
                for (int i = s0 + 1; i < s1; ++i)
                {
                    lo = std::min (lo, d[i]);
                    hi = std::max (hi, d[i]);
                }
                mm[(size_t) b] = { lo, hi };
            }
            cache.push_back (std::move (mm));
        }
    }

    void timerCallback() override
    {
        if (tap != nullptr)
        {
            // Follow the tap's sample rate if the owner re-prepared it.
            if (tap->getSampleRate() > 0.0 && tap->getSampleRate() != sampleRate)
                sampleRate = tap->getSampleRate();
            repaint();
        }
    }

    //==========================================================================
    juce::AudioBuffer<float> audio;                     // buffer/file source
    std::vector<std::vector<std::pair<float, float>>> cache;    // per channel
    float dataPeak = 1.0f;

    WaveformTap* tap = nullptr;                         // realtime source
    std::vector<float> tapScratch;

    double sampleRate = 44100.0;
    double timeOffsetS = 0.0;                           // sample 0 shows at -offset

    double viewStartS = 0.0, viewLengthS = 1.0;         // display-time window
    float ampRange = 1.0f;                              // plot spans +/- this

    Colours colours;
    std::vector<juce::Colour> channelColours;
    juce::StringArray channelNames;
    std::vector<Marker> markers;
    std::vector<Region> regions;

    // Legend toggles (drawing only) + the hit rects paint() lays out.
    std::vector<bool> channelHidden;
    mutable std::vector<std::pair<juce::Rectangle<int>, int>> legendHits;

    bool dragging = false;
    float dragStartX = 0.0f;
    double dragStartViewS = 0.0;

    juce::Point<float> cursorPos;
    bool cursorInPlot = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformDisplay)
};

} // namespace fxme
