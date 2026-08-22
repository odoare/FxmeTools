/*
  ------------------------------------------------------------------------------
    SpectrumDisplay.cpp

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include "SpectrumDisplay.h"

namespace fxme
{

void SpectrumDisplay::timerCallback()
{
    const double sr = sampleRateProvider != nullptr ? sampleRateProvider() : 0.0;
    bool any = false;

    for (auto& tr : traces)
    {
        const bool en = tr.cfg.tap != nullptr && tr.cfg.tap->isEnabled();
        if (en != tr.enabled)
        {
            tr.enabled = en;
            tr.smoothedDb.fill (-120.0f);
            any = true;
        }
        if (! en)
            continue;

        any = true;
        const float weight = avgOn ? 1.0f / (float) juce::jmax (1, nAvg) : 1.0f;
        analyzer.update (*tr.cfg.tap, tr.smoothedDb, sr, mode, weight);
    }

    if (any)
        repaint();
}

void SpectrumDisplay::setFftOrder (int order)
{
    const int clamped = juce::jlimit (spectrumMinFftOrder, spectrumMaxFftOrder, order);
    if (clamped == fftOrder)
        return;

    fftOrder = clamped;
    analyzer.setFftSize (1 << fftOrder);
    restartAveraging();
    repaint();
}

void SpectrumDisplay::mouseDown (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();
    dragging = false;

    // A click on a legend entry shows/hides that trace.
    for (const auto& hit : legendHits)
        if (hit.first.contains (p))
        {
            auto& tr = traces[(size_t) hit.second];
            tr.userVisible = ! tr.userVisible;
            repaint();
            return;
        }

    // Badges first; each click restarts the averaging.
    if (detectorBadgeBounds().contains (p))
    {
        mode = mode == Mode::peak ? Mode::average : Mode::peak;
        restartAveraging();
        repaint();
        return;
    }
    if (fftBadgeBounds().contains (p))
    {
        // Cycle the window size through the supported orders, unless the host
        // has pinned it.
        if (! fftLocked)
            setFftOrder (fftOrder >= spectrumMaxFftOrder ? spectrumMinFftOrder : fftOrder + 1);
        return;
    }
    if (avgBadgeBounds().contains (p))
    {
        avgOn = ! avgOn;
        restartAveraging();
        repaint();
        return;
    }
    if (nBadgeBounds().contains (p))
    {
        static const int opts[] = { 2, 4, 8, 16, 32 };
        int i = 0;
        while (i < 4 && opts[i] < nAvg) ++i;        // index of current (or next) value
        nAvg = opts[(i + 1) % 5];
        avgOn = true;                                // choosing N implies averaging on
        restartAveraging();
        repaint();
        return;
    }

    // Otherwise begin a pan (both axes) if the press is inside the plot.
    dragging = getPlotArea().contains (e.position);
    dragStartY = e.position.y;
    dragStartMin = minDb;
    dragStartMax = maxDb;
    dragStartX = e.position.x;
    dragStartFMin = viewFMin;
    dragStartFMax = viewFMax;
}

void SpectrumDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging)
        return;

    const auto plot = getPlotArea();

    // Horizontal: pan the (log) frequency window by the dragged decades.
    const float ratio = dragStartFMax / dragStartFMin;
    const float shift = std::exp (std::log (ratio)
                                  * (dragStartX - e.position.x) / juce::jmax (1.0f, plot.getWidth()));
    setFreqWindow (dragStartFMin * shift, dragStartFMax * shift);

    // Vertical: pan the dB window.
    const float span = dragStartMax - dragStartMin;
    const float dbPerPx = span / juce::jmax (1.0f, plot.getHeight());
    setDbWindow (dragStartMin + (e.position.y - dragStartY) * dbPerPx, span);
}

void SpectrumDisplay::mouseMove (const juce::MouseEvent& e)
{
    cursorPos = e.position;
    const bool in = getPlotArea().contains (e.position) && ! overBadge (e.getPosition());
    if (in != cursorInPlot || in)
    {
        cursorInPlot = in;
        repaint();
    }
}

void SpectrumDisplay::mouseExit (const juce::MouseEvent&)
{
    if (cursorInPlot)
    {
        cursorInPlot = false;
        repaint();
    }
}

void SpectrumDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (getPlotArea().contains (e.position) && ! overBadge (e.getPosition()))
    {
        minDb = defaultMinDb;
        maxDb = defaultMaxDb;
        setFreqWindow (SpectrumAnalyzer::fMin, SpectrumAnalyzer::fMax);
        repaint();
    }
}

void SpectrumDisplay::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    auto plot = getPlotArea();
    if (! plot.contains (e.position))
        return;

    const float factor = w.deltaY > 0.0f ? 0.85f : 1.0f / 0.85f;

    // Ctrl (or cmd): zoom the frequency axis around the cursor, keeping the
    // frequency under it pinned.
    if (e.mods.isCtrlDown() || e.mods.isCommandDown())
    {
        const float fAtX = xToFreq (e.position.x, plot);
        const float ratio = viewFMax / viewFMin;
        const float newRatio = std::pow (ratio, factor);
        const float rel = std::log (fAtX / viewFMin) / std::log (ratio);
        const float newLo = fAtX / std::pow (newRatio, rel);
        setFreqWindow (newLo, newLo * newRatio);
        return;
    }

    const float span   = maxDb - minDb;
    const float dbAtY   = juce::jmap (e.position.y, plot.getBottom(), plot.getY(), minDb, maxDb);
    const float newSpan = juce::jlimit (dbMinSpan, dbCeil - dbFloor, span * factor);
    const float frac    = span > 0.0f ? (dbAtY - minDb) / span : 0.5f;
    setDbWindow (dbAtY - frac * newSpan, newSpan);
}

std::vector<float> SpectrumDisplay::freqTicks() const
{
    std::vector<float> t;
    const double firstDec = std::floor (std::log10 ((double) viewFMin));
    const double lastDec  = std::log10 ((double) viewFMax);
    for (double dec = firstDec; dec <= lastDec + 1.0; dec += 1.0)
        for (double m : { 1.0, 2.0, 5.0 })
        {
            const double f = m * std::pow (10.0, dec);
            if (f >= (double) viewFMin * 0.999 && f <= (double) viewFMax * 1.001)
                t.push_back ((float) f);
        }
    return t;
}

void SpectrumDisplay::drawBadge (juce::Graphics& g, juce::Rectangle<int> r,
                                 const juce::String& text, bool active) const
{
    auto b = r.toFloat();
    g.setColour (colours.plotBackground.withAlpha (0.6f));
    g.fillRoundedRectangle (b, 3.0f);
    g.setColour (colours.panelLine);
    g.drawRoundedRectangle (b, 3.0f, 1.0f);
    g.setColour (active ? colours.text : colours.dimText);
    g.setFont (10.0f);
    g.drawText (text, b, juce::Justification::centred);
}

void SpectrumDisplay::drawCursorReadout (juce::Graphics& g, juce::Rectangle<float> plot) const
{
    if (! cursorInPlot)
        return;

    // Invert the axis mappings to get frequency / level under the pointer.
    const float f  = xToFreq (cursorPos.x, plot);
    const float db = juce::jmap (juce::jlimit (plot.getY(), plot.getBottom(), cursorPos.y),
                                 plot.getBottom(), plot.getY(), minDb, maxDb);
    const float shown = splCalibrated ? db + splOffset : db;

    const juce::String txt =
        (f >= 1000.0f ? juce::String (f / 1000.0f, 2) + " kHz"
                      : juce::String (juce::roundToInt (f)) + " Hz")
        + "   " + juce::String (shown, 1) + (splCalibrated ? " dB SPL" : " dBFS");

    g.setFont (10.0f);
    const int tw = (int) juce::GlyphArrangement::getStringWidth (juce::Font (juce::FontOptions (10.0f)), txt) + 12;
    juce::Rectangle<int> box ((int) plot.getRight() - tw - 2, (int) plot.getY() + 2, tw, 14);
    g.setColour (colours.plotBackground.withAlpha (0.75f));
    g.fillRoundedRectangle (box.toFloat(), 3.0f);
    g.setColour (colours.panelLine);
    g.drawRoundedRectangle (box.toFloat(), 3.0f, 1.0f);
    g.setColour (colours.text);
    g.drawText (txt, box, juce::Justification::centred);
}

void SpectrumDisplay::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour (colours.plotBackground);
    g.fillRoundedRectangle (bounds, 6.0f);

    const auto plot = getPlotArea();

    // Frequency grid (1-2-5 ticks across the window) with labels under the plot.
    g.setFont (10.0f);
    for (float f : freqTicks())
    {
        const float x = freqToX (f, plot);
        g.setColour (colours.grid);
        g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
        g.setColour (colours.dimText);
        g.drawText (f >= 1000.0f ? juce::String (f / 1000.0f) + "k" : juce::String ((int) f),
                    (int) x - 14, (int) plot.getBottom() + 2, 28, 12, juce::Justification::centred);
    }

    // Level grid: a labelled line every 10 dB and a faint secondary line every
    // 5 dB. Labels in dBFS, or in dB SPL when a calibration has been entered
    // (the curves keep their dBFS position).
    const int firstDb = (int) std::ceil  (minDb / 5.0f) * 5;
    const int lastDb  = (int) std::floor (maxDb / 5.0f) * 5;
    for (int db = firstDb; db <= lastDb; db += 5)
    {
        const float y = dbToY ((float) db, plot);
        const bool labelled = (db % 10) == 0;

        g.setColour (labelled ? (db == 0 ? colours.gridZero : colours.grid)
                              : colours.grid.withMultipliedAlpha (0.5f));
        g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());

        if (labelled)
        {
            g.setColour (colours.dimText);
            const int label = splCalibrated ? juce::roundToInt ((float) db + splOffset) : db;
            g.drawText (juce::String (label), 2, (int) y - 6, 22, 12, juce::Justification::centredRight);
        }
    }

    if (splCalibrated)
    {
        g.setColour (colours.dimText);
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        g.drawText ("dB SPL", (int) plot.getRight() - 62, (int) plot.getY() + 2, 60, 12,
                    juce::Justification::centredRight);
    }

    // Traces, clipped to the plot: when zoomed in, out-of-window points must
    // not paint over the axis labels or the legend.
    {
        juce::Graphics::ScopedSaveState clipState (g);
        g.reduceClipRegion (plot.toNearestInt());

        paintBehindTraces (g, plot);

        for (auto& tr : traces)
        {
            if (! tr.enabled || ! tr.userVisible)
                continue;

            juce::Path path;
            bool started = false;
            for (int p = 0; p < SpectrumAnalyzer::numPoints; ++p)
            {
                const float freq = SpectrumAnalyzer::pointFreq (p);
                const float off  = magnitudeOffsetDb != nullptr ? magnitudeOffsetDb (freq) : 0.0f;
                const float x = freqToX (freq, plot);
                const float y = dbToY (juce::jlimit (minDb, maxDb, tr.smoothedDb[(size_t) p] + off), plot);
                if (! started) { path.startNewSubPath (x, y); started = true; }
                else           path.lineTo (x, y);
            }
            g.setColour (tr.cfg.colour);
            g.strokePath (path, juce::PathStrokeType (tr.cfg.thickness));
        }

        paintOverTraces (g, plot);
    }

    // Legend: one clickable entry per running trace (mouseDown scans
    // legendHits); a hidden trace's swatch and label are dimmed.
    legendHits.clear();
    int legendX = (int) plot.getX() + 4;
    g.setFont (11.0f);
    for (int i = 0; i < (int) traces.size(); ++i)
    {
        auto& tr = traces[(size_t) i];
        if (! tr.enabled)
            continue;

        const auto label = tr.cfg.label != nullptr ? tr.cfg.label() : juce::String();
        if (label.isEmpty())
            continue;

        const int w = 14 + (int) juce::GlyphArrangement::getStringWidth (juce::Font (juce::FontOptions (11.0f)), label);
        g.setColour (tr.userVisible ? tr.cfg.colour : tr.cfg.colour.withAlpha (0.3f));
        g.fillRect (legendX, (int) plot.getY() + 3, 8, 8);
        g.setColour (tr.userVisible ? colours.text : colours.dimText.withAlpha (0.6f));
        g.drawText (label, legendX + 11, (int) plot.getY(), w, 14, juce::Justification::centredLeft);
        legendHits.push_back ({ { legendX - 2, (int) plot.getY(), w + 13, 14 }, i });
        legendX += w + 10;
    }

    // Clickable badges: window size + temporal averaging (bottom-left),
    // per-point detector avg/peak (bottom-right).
    drawBadge (g, fftBadgeBounds(), "fft " + juce::String (analyzer.getFftSize()), ! fftLocked);
    drawBadge (g, avgBadgeBounds(), "avg", avgOn);
    drawBadge (g, nBadgeBounds(),   "N " + juce::String (nAvg), avgOn);
    drawBadge (g, detectorBadgeBounds(), mode == Mode::peak ? "peak" : "avg", true);

    // Cursor frequency / level read-out (top-right), drawn over the SPL tag.
    drawCursorReadout (g, plot);

    g.setColour (colours.panelLine);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, 1.0f);
}

} // namespace fxme
