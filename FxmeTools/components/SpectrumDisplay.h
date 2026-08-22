/*
  ------------------------------------------------------------------------------
    SpectrumDisplay.h

    Reusable spectrum view: draws a log-frequency / dB grid and one stroked
    trace per registered tap, running the shared SpectrumAnalyzer on a GUI
    timer. Supports many taps (e.g. a monitoring matrix analyzer) or one (e.g. a
    calibration mic analyzer). When an SPL calibration is supplied the vertical
    axis is relabelled in dB SPL (= dBFS + offset).

    Add/remove curves with addTrace()/clearTraces(); the per-point detector
    (avg/peak), FFT window size and temporal averaging are user-clickable
    badges. Palette is injected via setColours() (defaults to a dark theme).

    Mouse: wheel zooms the dB axis around the cursor, ctrl+wheel zooms the
    frequency axis around it, drag pans both, double-click resets both. The
    legend is clickable: a click on a trace's entry hides/shows that trace.

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>
#include <FxmeTools/dsp/SpectrumAnalyzer.h>
#include <FxmeTools/dsp/SpectrumTap.h>

namespace fxme
{

class SpectrumDisplay : public juce::Component,
                        private juce::Timer
{
public:
    /** Palette for the grid, labels and chrome. Trace colours are per-trace
        (see TraceConfig::colour). Defaults to a dark theme. */
    struct Colours
    {
        juce::Colour plotBackground { 0xff000000 };   // plot / badge background
        juce::Colour grid           { 0x66555555 };   // faint grid line
        juce::Colour gridZero       { 0xcc555555 };   // 0 dB / 0 deg grid line
        juce::Colour text           { 0xffd8d8e0 };   // labels / active badge
        juce::Colour dimText        { 0xff9a9aa8 };   // axis labels / inactive badge
        juce::Colour panelLine      { 0xff3a3a4c };   // outlines
    };

    struct TraceConfig
    {
        SpectrumTap* tap = nullptr;
        juce::Colour colour { juce::Colours::white };
        std::function<juce::String()> label;       // evaluated at paint time
        float thickness = 1.4f;
    };

    SpectrumDisplay()
    {
        analyzer.setFftSize (1 << fftOrder);
        startTimerHz (25);
    }

    void setColours (Colours c) { colours = c; repaint(); }
    const Colours& getColours() const noexcept { return colours; }

    /** Registers a tap to display. Traces draw only while their tap is enabled. */
    void addTrace (TraceConfig cfg)
    {
        traces.push_back ({ std::move (cfg), false, {} });
        traces.back().smoothedDb.fill (-120.0f);
    }

    void clearTraces() { traces.clear(); }

    void setDbRange (float lo, float hi)
    {
        minDb = defaultMinDb = lo;      // also the double-click "reset" view
        maxDb = defaultMaxDb = hi;
    }

    /** Frequency window (log axis), clamped to the analyzer's full range. */
    void setFreqWindow (float lo, float hi)
    {
        const float fullLo = SpectrumAnalyzer::fMin, fullHi = SpectrumAnalyzer::fMax;
        hi = juce::jlimit (fullLo * 1.26f, fullHi, hi);
        lo = juce::jlimit (fullLo, hi / 1.26f, lo);
        viewFMin = lo;
        viewFMax = hi;
        repaint();
    }

    float getViewLowHz() const noexcept     { return viewFMin; }
    float getViewHighHz() const noexcept    { return viewFMax; }

    using Mode = SpectrumAnalyzer::Mode;
    void setSpectrumMode (Mode m) { mode = m; repaint(); }
    Mode getSpectrumMode() const  { return mode; }

    /** Analysis window size, as an FFT order (clamped to the supported range).
        Worth pinning when something else in the plugin measures the same
        signal and has to agree with what is drawn: a spectral gate threshold,
        say, only sits where it looks like it sits when the view and the DSP
        run the same window. */
    void setFftOrder (int order);
    int  getFftOrder() const noexcept       { return fftOrder; }

    /** Stops the window-size badge responding to clicks (it draws dimmed), so
        a pinned size cannot be changed from the GUI. */
    void setFftSizeLocked (bool shouldBeLocked) { fftLocked = shouldBeLocked; repaint(); }
    bool isFftSizeLocked() const noexcept       { return fftLocked; }

    /** When calibrated, the vertical axis is labelled in dB SPL = dBFS + offset
        (the plotted curves do not move, only the numbers). */
    void setSplCalibration (bool calibrated, float offsetDb)
    {
        splCalibrated = calibrated;
        splOffset = offsetDb;
        repaint();
    }

    std::function<double()> sampleRateProvider;

    // Optional per-frequency dB offset added to every plotted trace (e.g. a
    // microphone calibration: pass -micDeviationDb(f) to show corrected level).
    // Evaluated at paint time; leave null for no offset.
    std::function<float (float freqHz)> magnitudeOffsetDb;

    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;

protected:
    //==========================================================================
    // Everything a subclass needs to draw and hit-test in the plot's own
    // coordinates. SpectrumRegionEditor is built on these.

    /** Drawn after the grid and before the traces, clipped to the plot: for
        anything that belongs underneath the curves (band fills, markers). */
    virtual void paintBehindTraces (juce::Graphics&, juce::Rectangle<float> /*plotArea*/) {}

    /** Drawn after the traces and before the legend and badges, clipped to the
        plot: for handles and anything that must stay legible over a curve. */
    virtual void paintOverTraces (juce::Graphics&, juce::Rectangle<float> /*plotArea*/) {}

    juce::Rectangle<float> getPlotArea() const
    {
        return getLocalBounds().toFloat().reduced (8.0f)
                   .withTrimmedLeft (18.0f).withTrimmedBottom (14.0f);
    }

    float freqToX (float f, juce::Rectangle<float> r) const
    {
        return r.getX() + r.getWidth() * std::log (f / viewFMin) / std::log (viewFMax / viewFMin);
    }

    float xToFreq (float x, juce::Rectangle<float> r) const
    {
        const float rel = juce::jlimit (0.0f, 1.0f, (x - r.getX()) / juce::jmax (1.0f, r.getWidth()));
        return viewFMin * std::exp (rel * std::log (viewFMax / viewFMin));
    }

    float dbToY (float db, juce::Rectangle<float> r) const
    {
        return juce::jmap (db, minDb, maxDb, r.getBottom(), r.getY());
    }

    float yToDb (float y, juce::Rectangle<float> r) const
    {
        return juce::jmap (juce::jlimit (r.getY(), r.getBottom(), y),
                           r.getBottom(), r.getY(), minDb, maxDb);
    }

    /** True when the point is over one of the clickable badges, which a
        subclass must leave to the base class. */
    bool overBadge (juce::Point<int> p) const
    {
        return detectorBadgeBounds().contains (p) || fftBadgeBounds().contains (p)
            || avgBadgeBounds().contains (p) || nBadgeBounds().contains (p);
    }

    /** True when the point is over a legend entry (same reasoning). Only valid
        once the component has painted at least once. */
    bool overLegend (juce::Point<int> p) const
    {
        for (const auto& hit : legendHits)
            if (hit.first.contains (p))
                return true;
        return false;
    }

private:
    struct Trace
    {
        TraceConfig cfg;
        bool enabled;                   // tap is running
        std::array<float, (size_t) SpectrumAnalyzer::numPoints> smoothedDb;
        bool userVisible = true;        // legend toggle (drawing only)
    };

    void timerCallback() override;

    // 1-2-5 frequency ticks across the current window.
    std::vector<float> freqTicks() const;

    // Clickable badges. The detector (avg/peak) stays bottom-right; the window
    // size and temporal-averaging controls sit bottom-left.
    juce::Rectangle<int> detectorBadgeBounds() const
    {
        auto plot = getPlotArea();
        return { (int) plot.getRight() - 50, (int) plot.getBottom() - 18, 46, 14 };
    }
    juce::Rectangle<int> fftBadgeBounds() const
    {
        auto plot = getPlotArea();
        return { (int) plot.getX() + 4, (int) plot.getBottom() - 18, 58, 14 };
    }
    juce::Rectangle<int> avgBadgeBounds() const
    {
        return fftBadgeBounds().translated (62, 0).withWidth (38);
    }
    juce::Rectangle<int> nBadgeBounds() const
    {
        return avgBadgeBounds().translated (42, 0).withWidth (40);
    }

    void drawBadge (juce::Graphics& g, juce::Rectangle<int> r,
                    const juce::String& text, bool active) const;

    void restartAveraging()
    {
        for (auto& tr : traces)
            tr.smoothedDb.fill (-120.0f);
    }

    // Apply a [min, min+span] dB window, clamped to fit within [floor, ceil].
    void setDbWindow (float newMin, float span)
    {
        span   = juce::jlimit (dbMinSpan, dbCeil - dbFloor, span);
        newMin = juce::jlimit (dbFloor, dbCeil - span, newMin);
        minDb = newMin;
        maxDb = newMin + span;
        repaint();
    }

    std::vector<Trace> traces;
    SpectrumAnalyzer analyzer;
    Colours colours;

    float minDb = -100.0f, maxDb = 10.0f;
    float defaultMinDb = -100.0f, defaultMaxDb = 10.0f;
    float viewFMin = SpectrumAnalyzer::fMin, viewFMax = SpectrumAnalyzer::fMax;
    bool  splCalibrated = false;
    float splOffset = 0.0f;
    Mode  mode = Mode::average;

    int  fftOrder = spectrumFftOrder;           // window size = 1 << fftOrder
    bool fftLocked = false;                     // badge ignores clicks
    bool avgOn = true;                          // temporal averaging
    int  nAvg  = 4;                             // averaged over ~nAvg frames

    // Zoom/pan (drag) state, dB and frequency axes.
    static constexpr float dbFloor = -200.0f, dbCeil = 200.0f, dbMinSpan = 10.0f;
    bool dragging = false;
    float dragStartY = 0.0f, dragStartMin = -100.0f, dragStartMax = 10.0f;
    float dragStartX = 0.0f;
    float dragStartFMin = SpectrumAnalyzer::fMin, dragStartFMax = SpectrumAnalyzer::fMax;

    // Legend hit rects (trace index), laid out by paint().
    mutable std::vector<std::pair<juce::Rectangle<int>, int>> legendHits;

    // Cursor read-out (frequency / level at the pointer), shown top-right.
    juce::Point<float> cursorPos;
    bool cursorInPlot = false;
    void drawCursorReadout (juce::Graphics& g, juce::Rectangle<float> plot) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumDisplay)
};

} // namespace fxme
