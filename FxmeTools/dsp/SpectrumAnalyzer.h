/*
  ------------------------------------------------------------------------------
    SpectrumAnalyzer.h

    Spectrum analysis logic shared by every spectrum view. Given a SpectrumTap,
    it snapshots the latest fftSize samples, windows and FFTs them, maps the
    magnitude onto a log-frequency grid and exponentially averages the result
    into a dB curve. Pure DSP/maths: no GUI, no ownership of the taps.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>
#include "SpectrumTap.h"

namespace fxme
{

class SpectrumAnalyzer
{
public:
    static constexpr int   numPoints = 512;             // points along the log-f axis
    static constexpr float fMin = 20.0f, fMax = 20000.0f;

    /** How the FFT bins falling on one display point are collapsed.
        - average: RMS (mean power) of the bins — broadband noise reads flat.
        - peak:    loudest bin — better for spotting narrow tones, but biases
                   broadband noise upward toward HF (more bins per point). */
    enum class Mode { average, peak };

    SpectrumAnalyzer();

    /** Display frequency (Hz) of grid point p. */
    static float pointFreq (int p);

    /** FFT window size (number of samples). Clamped to a power of two in
        [1<<spectrumMinFftOrder, 1<<spectrumMaxFftOrder]; rebuilds on change. */
    void setFftSize (int sizePow2);
    int getFftSize() const noexcept             { return fftSize; }

    /** Windowed FFT of the tap's latest snapshot, mapped onto the log grid and
        blended into smoothedDb. newWeight is the weight of this frame (1 = no
        temporal averaging; 1/N ≈ averaging over the last N frames). */
    void update (SpectrumTap& tap, std::array<float, numPoints>& smoothedDb,
                 double sampleRate, Mode mode, float newWeight);

private:
    void rebuild (int order);

    std::unique_ptr<juce::dsp::FFT> fft;
    int fftOrder = spectrumFftOrder;
    int fftSize  = spectrumFftSize;
    std::array<float, (size_t) spectrumMaxFftSize> window;
    std::array<float, (size_t) (2 * spectrumMaxFftSize)> fftData {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumAnalyzer)
};

} // namespace fxme
