/*
  ------------------------------------------------------------------------------
    CircularSpectrum.h

    A spectrum drawn as a round gauge: radial bars around a 270 degree dial,
    20 Hz at the lower left to 20 kHz at the lower right on a log scale, each
    bar's length its level, with falling peak markers. Made for dashboards
    (Camshaft puts it where a car has its speedometer).

    It reads a SpectrumTap (core dsp/SpectrumTap.h) through the shared
    SpectrumAnalyzer on a GUI timer, enables the tap while it exists and does
    no work while it is not showing. The level scale has a tilt (dB per
    octave around 1 kHz, 3 by default) so that music, whose spectrum falls
    with frequency, reads level across the dial.

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

class CircularSpectrum : public juce::Component,
                         public juce::SettableTooltipClient,
                         private juce::Timer
{
public:
    struct Colours
    {
        juce::Colour face     { 0xff0e0f12 };
        juce::Colour rim      { 0xff383b45 };
        juce::Colour ticks    { 0xff9a978f };
        juce::Colour barLow   { 0xff4fc3c7 };
        juce::Colour barHigh  { 0xffff7a1a };
        juce::Colour peak     { 0xffe6e4de };
        juce::Colour caption  { 0xff6e6c66 };
    };

    /** `sampleRate` is asked at each refresh (the host's rate may change). */
    CircularSpectrum (SpectrumTap& tapToRead, std::function<double()> sampleRate)
        : tap (tapToRead), getSampleRate (std::move (sampleRate))
    {
        smoothed.fill (-150.0f);
        bars.fill (0.0f);
        peaks.fill (0.0f);
        peakAge.fill (0.0f);
        analyzer.setFftSize (4096);
        tap.setEnabled (true);
        startTimerHz (30);
    }

    ~CircularSpectrum() override
    {
        stopTimer();
        tap.setEnabled (false);
    }

    void setColours (Colours c)                    { colours = c; repaint(); }
    void setRange (float minDbIn, float maxDbIn)   { minDb = minDbIn; maxDb = juce::jmax (minDbIn + 6.0f, maxDbIn); }
    void setTilt (float dbPerOctave)               { tilt = dbPerOctave; }
    void setCaption (const juce::String& text)     { caption = text; repaint(); }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat().reduced (2.0f);
        const float r = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f;
        const auto c = b.getCentre();

        g.setColour (colours.face);
        g.fillEllipse (juce::Rectangle<float> (2.0f * r, 2.0f * r).withCentre (c));
        g.setColour (colours.rim);
        g.drawEllipse (juce::Rectangle<float> (2.0f * r - 2.0f, 2.0f * r - 2.0f).withCentre (c), 1.5f);

        const float inner = r * 0.36f, outer = r * 0.80f;

        // Frequency ticks on the rim, labels inside.
        g.setFont (juce::Font (juce::FontOptions (juce::jmax (8.0f, r * 0.11f))));
        for (int hzi : { 50, 100, 200, 500, 1000, 2000, 5000, 10000 })
        {
            const float hz = (float) hzi;
            const float a = angleFor (hz);
            const bool major = hzi == 100 || hzi == 1000 || hzi == 10000;
            g.setColour (colours.ticks.withAlpha (major ? 0.9f : 0.5f));
            g.drawLine ({ c.getPointOnCircumference (r * 0.86f, a), c.getPointOnCircumference (r * (major ? 0.95f : 0.91f), a) },
                        major ? 1.4f : 1.0f);
            if (major)
            {
                const auto p = c.getPointOnCircumference (inner * 0.70f, a);
                const auto label = hzi >= 1000 ? juce::String (hzi / 1000) + "k" : juce::String (hzi);
                g.drawText (label, juce::Rectangle<float> (r * 0.4f, r * 0.14f).withCentre (p), juce::Justification::centred);
            }
        }

        // Bars.
        const float span = (endAngle - startAngle) / (float) numBars;
        const float width = juce::jmax (1.5f, span * (inner + outer) * 0.5f * 0.62f);
        for (int i = 0; i < numBars; ++i)
        {
            const float a = startAngle + span * ((float) i + 0.5f);
            const float v = bars[(size_t) i];
            const auto colour = colours.barLow.interpolatedWith (colours.barHigh, v);
            if (v > 0.001f)
            {
                g.setColour (colour);
                g.drawLine ({ c.getPointOnCircumference (inner, a), c.getPointOnCircumference (inner + (outer - inner) * v, a) }, width);
            }
            const float p = peaks[(size_t) i];
            if (p > 0.01f)
            {
                g.setColour (colours.peak.withAlpha (0.8f));
                const float rp = inner + (outer - inner) * p;
                g.drawLine ({ c.getPointOnCircumference (rp, a), c.getPointOnCircumference (rp + 1.5f, a) }, width);
            }
        }

        if (caption.isNotEmpty())
        {
            g.setColour (colours.caption);
            g.setFont (juce::Font (juce::FontOptions (juce::jmax (8.0f, r * 0.12f), juce::Font::bold)));
            g.drawText (caption, juce::Rectangle<float> (r, r * 0.2f).withCentre ({ c.x, c.y + r * 0.62f }),
                        juce::Justification::centred);
        }
    }

private:
    static constexpr int numBars = 48;
    static constexpr float startAngle = -2.356194f, endAngle = 2.356194f;   // radians from straight up

    static float angleFor (float hz)
    {
        const float t = std::log (hz / SpectrumAnalyzer::fMin) / std::log (SpectrumAnalyzer::fMax / SpectrumAnalyzer::fMin);
        return startAngle + (endAngle - startAngle) * juce::jlimit (0.0f, 1.0f, t);
    }

    void timerCallback() override
    {
        if (! isShowing())
            return;

        const double sr = getSampleRate ? getSampleRate() : 44100.0;
        analyzer.update (tap, smoothed, sr > 0.0 ? sr : 44100.0, SpectrumAnalyzer::Mode::peak, 0.4f);

        constexpr int perBar = SpectrumAnalyzer::numPoints / numBars;
        bool changed = false;
        for (int i = 0; i < numBars; ++i)
        {
            float db = -200.0f;
            for (int p = i * perBar; p < (i + 1) * perBar; ++p)
                db = juce::jmax (db, smoothed[(size_t) p]);
            const float hz = SpectrumAnalyzer::pointFreq (i * perBar + perBar / 2);
            db += tilt * std::log2 (hz / 1000.0f);
            const float target = juce::jlimit (0.0f, 1.0f, (db - minDb) / (maxDb - minDb));

            // Up at once, down at about a full scale per second.
            auto& v = bars[(size_t) i];
            const float next = target > v ? target : juce::jmax (target, v - 0.035f);
            if (std::abs (next - v) > 1.0e-4f)
                changed = true;
            v = next;

            auto& pk = peaks[(size_t) i];
            auto& age = peakAge[(size_t) i];
            if (v >= pk)
            {
                pk = v;
                age = 0.0f;
            }
            else if ((age += 1.0f / 30.0f) > 0.8f)
            {
                pk = juce::jmax (v, pk - 0.02f);
                changed = true;
            }
        }
        if (changed)
            repaint();
    }

    SpectrumTap& tap;
    std::function<double()> getSampleRate;
    SpectrumAnalyzer analyzer;
    std::array<float, SpectrumAnalyzer::numPoints> smoothed {};
    std::array<float, numBars> bars {}, peaks {}, peakAge {};
    Colours colours;
    float minDb = -90.0f, maxDb = -12.0f, tilt = 3.0f;
    juce::String caption;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CircularSpectrum)
};

} // namespace fxme
