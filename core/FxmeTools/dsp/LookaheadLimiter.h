/*
  ------------------------------------------------------------------------------
    dsp/LookaheadLimiter.h

    A brick-wall safety limiter for the end of a chain: the output never
    exceeds the ceiling (sample peaks), with a short look-ahead so the gain is
    already down when a peak arrives, and a smooth release.

    How the gain is guaranteed: for each sample the gain it needs is
    min (1, ceiling / |x|) (stereo linked: the louder channel). A sliding
    minimum over the look-ahead window L, then a moving average over the same
    L, gives a smooth curve that is never above the needed gain of the sample
    L - 1 earlier; the signal is delayed by L - 1 samples to meet it. On top,
    a one-pole release lets the gain come back up slowly; it may only lower
    the gain the window computed, never raise it.

    The window min is an O(1) monotonic deque in fixed arrays. Latency is
    L - 1 samples (report it to the host). getGainReductionDb() is atomic for
    a meter or a warning light. Header-only, allocation in prepare() only.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/util/Math.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace fxme
{

class LookaheadLimiter
{
public:
    void prepare (double sampleRate, float lookaheadMs = 1.0f, float releaseMs = 80.0f)
    {
        sr = sampleRate > 0.0 ? sampleRate : 44100.0;
        window = jmax (2, (int) std::ceil (lookaheadMs * 0.001 * sr));
        delay = window - 1;
        delayL.assign ((std::size_t) window, 0.0f);
        delayR.assign ((std::size_t) window, 0.0f);
        minDequeValue.assign ((std::size_t) window + 1, 1.0f);
        minDequeIndex.assign ((std::size_t) window + 1, 0);
        avgRing.assign ((std::size_t) window, 1.0f);
        releaseCoef = (float) std::exp (-1.0 / (0.001 * (double) releaseMs * sr));
        reset();
    }

    void reset() noexcept
    {
        std::fill (delayL.begin(), delayL.end(), 0.0f);
        std::fill (delayR.begin(), delayR.end(), 0.0f);
        std::fill (avgRing.begin(), avgRing.end(), 1.0f);
        avgSum = (double) window;
        head = tail = 0;
        index = 0;
        writePos = 0;
        gain = 1.0f;
        reduction.store (0.0f);
    }

    void setCeilingDb (float db) noexcept { ceiling = decibelsToGain (db); }

    /** Samples of latency added to the signal. */
    int getLatencySamples() const noexcept { return delay; }

    /** Current gain reduction in dB (0 or positive). Any thread. */
    float getGainReductionDb() const noexcept { return reduction.load (std::memory_order_relaxed); }

    /** In place, stereo (right may be null for mono). */
    void process (float* left, float* right, int numSamples) noexcept
    {
        float maxReduction = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            const float xl = left[i];
            const float xr = right != nullptr ? right[i] : xl;
            const float peak = jmax (std::abs (xl), std::abs (xr));
            const float need = peak > ceiling ? ceiling / peak : 1.0f;

            // Sliding minimum over the last `window` needs.
            while (tail > head && minDequeValue[(std::size_t) (tail - 1) % minDequeValue.size()] >= need)
                --tail;
            minDequeValue[(std::size_t) tail % minDequeValue.size()] = need;
            minDequeIndex[(std::size_t) tail % minDequeIndex.size()] = index;
            ++tail;
            while (minDequeIndex[(std::size_t) head % minDequeIndex.size()] <= index - window)
                ++head;
            const float windowMin = minDequeValue[(std::size_t) head % minDequeValue.size()];

            // Moving average of the minimum.
            const std::size_t a = (std::size_t) (index % window);
            avgSum += (double) windowMin - (double) avgRing[a];
            avgRing[a] = windowMin;
            const float smoothed = (float) (avgSum / (double) window);

            // Release: come back up slowly, never above the computed gain.
            const float released = 1.0f - releaseCoef * (1.0f - gain);
            gain = jmin (smoothed, released);

            // Delay line.
            const float dl = delayL[(std::size_t) writePos];
            const float dr = delayR[(std::size_t) writePos];
            delayL[(std::size_t) writePos] = xl;
            delayR[(std::size_t) writePos] = xr;
            writePos = (writePos + 1) % delay;

            left[i] = dl * gain;
            if (right != nullptr)
                right[i] = dr * gain;

            maxReduction = jmax (maxReduction, -gainToDecibels (gain, -100.0f));
            ++index;
        }

        reduction.store (maxReduction, std::memory_order_relaxed);
    }

private:
    double sr = 44100.0;
    int window = 48, delay = 47;
    float ceiling = 0.966f;   // -0.3 dBFS
    float releaseCoef = 0.999f;
    float gain = 1.0f;

    std::vector<float> delayL, delayR;
    std::vector<float> minDequeValue;
    std::vector<long long> minDequeIndex;
    std::vector<float> avgRing;
    double avgSum = 0.0;
    long long head = 0, tail = 0, index = 0;
    int writePos = 0;

    std::atomic<float> reduction { 0.0f };
};

} // namespace fxme
