/*
  ------------------------------------------------------------------------------
    VuMeter.h

    Sliding-window level meter reporting RMS, instantaneous peak and a held
    maximum, all in dBFS. RMS is computed over a configurable window with a
    running sum (O(1) per sample); peak is the per-block max-abs; the held max
    accumulates until resetMax(). Levels are exposed through atomics so the GUI
    can read them from the message thread.

    Header-only, depends only on JUCE. Note this is a push-style DSP helper
    (feed it samples, read dB getters) — pair it with fxme::VuMeterComponent for
    display. For an RMS-only, realloc-free variant see fxme::RmsMeter.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <vector>
#include <cmath>

namespace fxme
{

class VuMeter
{
public:
    VuMeter() = default;

    void prepare (double sampleRate)
    {
        currentSampleRate = sampleRate;
        setWindowDuration (0.1);
        resetMax();
    }

    void process (const float* input, int numSamples)
    {
        float localMax = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            float sample = input[i];
            float sq = sample * sample;

            // Running sum: subtract old, add new.
            float oldSample = window[currentSample];
            currentSum -= (oldSample * oldSample);
            currentSum += sq;

            window[currentSample] = sample;

            currentSample = (currentSample + 1) % windowSize;

            float absSample = std::abs (sample);
            if (absSample > localMax)
                localMax = absSample;
        }

        // Prevent negative sum due to precision errors.
        if (currentSum < 0.0) currentSum = 0.0;

        // Update atomics once per block.
        rms = (float) std::sqrt (currentSum / windowSize);
        peak = localMax;

        float currentMax = maxLevel.load();
        if (localMax > currentMax)
            maxLevel.store (localMax);
    }

    // Getters return dB values
    float getRMS()  const { return juce::Decibels::gainToDecibels (rms.load()); }
    float getPeak() const { return juce::Decibels::gainToDecibels (peak.load()); }
    float getMax()  const { return juce::Decibels::gainToDecibels (maxLevel.load()); }
    void resetMax() { maxLevel = 0.0f; }

    void clear()
    {
        rms = 0.0f;
        peak = 0.0f;
        currentSum = 0.0;
        std::fill (window.begin(), window.end(), 0.0f);
    }

    void setWindowDuration (double duration)
    {
        windowSize = juce::jmax (1, juce::roundToInt (duration * currentSampleRate));
        window.assign (windowSize, 0.0f);
        currentSample = 0;
        currentSum = 0.0;
        rms = 0.0f;
        peak = 0.0f;
    }

private:
    double currentSampleRate = 44100.0;
    int windowSize = 1;
    int currentSample = 0;

    // Store linear values
    std::atomic<float> rms { 0.0f };
    std::atomic<float> peak { 0.0f };
    std::atomic<float> maxLevel { 0.0f };

    std::vector<float> window;
    double currentSum = 0.0;
};

} // namespace fxme
