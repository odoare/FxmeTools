/*
  ------------------------------------------------------------------------------
    SpectrumAnalyzer.cpp

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include "SpectrumAnalyzer.h"

namespace fxme
{

SpectrumAnalyzer::SpectrumAnalyzer()
{
    rebuild (spectrumFftOrder);
}

void SpectrumAnalyzer::rebuild (int order)
{
    fftOrder = juce::jlimit (spectrumMinFftOrder, spectrumMaxFftOrder, order);
    fftSize  = 1 << fftOrder;
    fft = std::make_unique<juce::dsp::FFT> (fftOrder);

    for (int i = 0; i < fftSize; ++i)
        window[(size_t) i] = 0.5f * (1.0f - std::cos (
            2.0f * juce::MathConstants<float>::pi * (float) i / (float) (fftSize - 1)));
}

void SpectrumAnalyzer::setFftSize (int sizePow2)
{
    const int order = juce::jlimit (spectrumMinFftOrder, spectrumMaxFftOrder,
                                    (int) std::round (std::log2 ((double) juce::jmax (1, sizePow2))));
    if (order != fftOrder)
        rebuild (order);
}

float SpectrumAnalyzer::pointFreq (int p)
{
    return fMin * std::pow (fMax / fMin, (float) p / (float) (numPoints - 1));
}

void SpectrumAnalyzer::update (SpectrumTap& tap, std::array<float, numPoints>& smoothedDb,
                               double sampleRate, Mode mode, float newWeight)
{
    tap.snapshot (fftData.data(), fftSize);
    for (int i = 0; i < fftSize; ++i)
        fftData[(size_t) i] *= window[(size_t) i];
    std::fill (fftData.begin() + fftSize, fftData.begin() + 2 * fftSize, 0.0f);

    fft->performFrequencyOnlyForwardTransform (fftData.data());

    const float binHz = (float) ((sampleRate > 0.0 ? sampleRate : 48000.0) / (double) fftSize);
    const float w = juce::jlimit (0.0f, 1.0f, newWeight);

    for (int p = 0; p < numPoints; ++p)
    {
        // Map this display point (and the span up to the next one) to FFT bins.
        // At HF many bins fall on one point: average their power (so broadband
        // noise stays flat) or take their peak, per the selected mode.
        const float f  = pointFreq (p);
        const int   b0 = juce::jlimit (1, fftSize / 2 - 1, (int) (f / binHz));
        const float f1 = pointFreq (juce::jmin (p + 1, numPoints - 1));
        const int   b1 = juce::jlimit (b0, fftSize / 2 - 1, (int) (f1 / binHz));

        float mag;
        if (mode == Mode::peak)
        {
            mag = 0.0f;
            for (int b = b0; b <= b1; ++b)
                mag = juce::jmax (mag, fftData[(size_t) b]);
        }
        else
        {
            double power = 0.0;
            for (int b = b0; b <= b1; ++b)
                power += (double) fftData[(size_t) b] * (double) fftData[(size_t) b];
            mag = (float) std::sqrt (power / (double) (b1 - b0 + 1));
        }

        const float db = juce::Decibels::gainToDecibels (mag * 2.0f / (float) fftSize, -120.0f);

        // Temporal averaging: exponential blend, weight w for the new frame.
        auto& s = smoothedDb[(size_t) p];
        s = s * (1.0f - w) + db * w;
    }
}

} // namespace fxme
