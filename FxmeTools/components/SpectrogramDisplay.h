/*
  ------------------------------------------------------------------------------
    SpectrogramDisplay.h

    Scrolling spectrogram over an fxme::SpectrumTap: time runs right to left,
    frequency bottom to top on a skewed (roughly logarithmic) axis, and the
    magnitude of each bin is the brightness of a single hue. Ported from
    FxmeJuceTools' SpectrogramComponent (itself after the JUCE "simple FFT"
    tutorial), with the same look, but fed by a tap instead of its own FIFO so
    the audio side is the same as for SpectrumDisplay: one tap can drive both.

    Each column is the magnitude spectrum of the last fftSize samples,
    normalised to its own loudest bin, then raised to a contrast exponent. A
    new column is drawn only when at least fftSize new samples have reached
    the tap, so the scroll speed is one column per FFT window (about 23 columns
    a second at 48 kHz with the default order 11) and the image freezes when
    the tap is disabled.

    Usage (editor):
        fxme::SpectrogramDisplay spectrogram;
        spectrogram.setTap (&processor.getSpectrumTap());
        spectrogram.setColour (juce::Colours::green);

    Mouse: vertical drag adjusts the contrast. Message thread only (the tap's
    push() side excepted).

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>
#include <FxmeTools/dsp/SpectrumTap.h>
#include <FxmeTools/util/Fft.h>
#include <cmath>
#include <cstdint>
#include <vector>

namespace fxme
{

class SpectrogramDisplay : public juce::Component,
                           private juce::Timer
{
public:
    /** historyColumns x frequencyRows is the resolution of the image, which
        is then stretched to the component. fftOrder must lie within the
        tap's range (spectrumMinFftOrder..spectrumMaxFftOrder). */
    explicit SpectrogramDisplay (int historyColumns = 128,
                                 int frequencyRows  = 512,
                                 int fftOrder       = 11)
        : image (juce::Image::RGB, juce::jmax (2, historyColumns), juce::jmax (2, frequencyRows), true),
          fftSize (1 << juce::jlimit (spectrumMinFftOrder, spectrumMaxFftOrder, fftOrder)),
          fft (juce::jlimit (spectrumMinFftOrder, spectrumMaxFftOrder, fftOrder)),
          window ((size_t) fftSize),
          fftData ((size_t) (2 * fftSize), 0.0f)
    {
        // Periodic Hann window, as SpectrumAnalyzer uses: the original
        // component had none, which smeared every partial over many rows.
        for (int i = 0; i < fftSize; ++i)
            window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize);

        startTimerHz (30);
    }

    /** The tap to read. The caller keeps ownership; nullptr detaches. The
        tap must be enabled by its owner for anything to show. */
    void setTap (SpectrumTap* newTap)
    {
        tap = newTap;
        lastColumnAt = tap != nullptr ? tap->getTotalPushed() : 0;
    }

    /** The hue of the image: each pixel is this colour at the bin's
        brightness. */
    void setColour (juce::Colour c)             { colour = c; }
    juce::Colour getColour() const noexcept     { return colour; }

    /** Exponent applied to the normalised magnitude: below 1 brings up the
        quiet bins, above 1 keeps only the strongest ones. */
    void setContrast (float newContrast)        { contrast = juce::jlimit (0.1f, 2.0f, newContrast); }
    float getContrast() const noexcept          { return contrast; }

    /** Blanks the image. */
    void clear()
    {
        image.clear (image.getBounds());
        repaint();
    }

    //==========================================================================
    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colours::black);
        g.fillRoundedRectangle (getLocalBounds().reduced (5).toFloat(), 10.0f);
        g.drawImage (image, getLocalBounds().reduced (10).toFloat());
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const int dy = e.getDistanceFromDragStartY();
        if (dy != 0)
            setContrast (contrast + (dy < 0 ? -0.005f : 0.005f));
    }

private:
    void timerCallback() override
    {
        if (tap == nullptr)
            return;

        const auto total = tap->getTotalPushed();
        if (total - lastColumnAt < (std::int64_t) fftSize)
            return;

        lastColumnAt = total;
        drawNextColumn();
        repaint();
    }

    void drawNextColumn()
    {
        const int right  = image.getWidth() - 1;
        const int height = image.getHeight();

        image.moveImageSection (0, 0, 1, 0, right, height);

        std::fill (fftData.begin(), fftData.end(), 0.0f);
        tap->snapshot (fftData.data(), fftSize);
        for (int i = 0; i < fftSize; ++i)
            fftData[(size_t) i] *= window[(size_t) i];

        fft.performFrequencyOnlyForwardTransform (fftData.data(), true);

        const int numBins = fftSize / 2;
        float maxLevel = 0.0f;
        for (int i = 0; i < numBins; ++i)
            maxLevel = juce::jmax (maxLevel, fftData[(size_t) i]);
        maxLevel = juce::jmax (maxLevel, 1.0e-5f);

        juce::Image::BitmapData bitmap { image, right, 0, 1, height, juce::Image::BitmapData::writeOnly };

        // Row 0 is the top of the image, i.e. the highest frequency. The skew
        // gives the low end most of the rows, as the original did.
        for (int y = 1; y < height; ++y)
        {
            const float skewed = 1.0f - std::exp (std::log ((float) y / (float) height) * 0.2f);
            const int bin = juce::jlimit (0, numBins, (int) (skewed * (float) numBins));
            const float level = std::pow (juce::jlimit (0.0f, 1.0f, fftData[(size_t) bin] / maxLevel), contrast);
            bitmap.setPixelColour (0, y, colour.withBrightness (level));
        }
    }

    juce::Image image;
    const int fftSize;
    RealFft fft;
    std::vector<float> window;
    std::vector<float> fftData;     // 2 * fftSize, the RealFft buffer layout

    SpectrumTap* tap = nullptr;
    std::int64_t lastColumnAt = 0;

    juce::Colour colour { juce::Colours::green };
    float contrast = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrogramDisplay)
};

} // namespace fxme
