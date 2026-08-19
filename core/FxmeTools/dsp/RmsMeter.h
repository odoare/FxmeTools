/*
  ------------------------------------------------------------------------------
    RmsMeter.h

    Sliding-window RMS level meter. On the audio thread it measures the RMS of
    an input over a configurable window (reported in dBFS) using a ring of
    squared samples with a running sum, so cost is O(1) per sample and the
    window length can change at runtime (which simply restarts accumulation).

    Allocation-free on the audio thread; controls are set from the message
    thread through atomics. Header-only, depends only on JUCE.

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

class RmsMeter
{
public:
    RmsMeter() = default;

    /** Sizes the ring for windows up to maxWindowSeconds. Message thread. */
    void prepare (double sampleRate, float maxWindowSeconds = 2.0f)
    {
        sr = sampleRate > 0.0 ? sampleRate : 44100.0;
        maxWindowSec = fxme::jmax (0.01f, maxWindowSeconds);
        ringCap = fxme::jmax (1, (int) (maxWindowSec * sr));
        sq.assign ((size_t) ringCap, 0.0f);
        reset();
    }

    /** Clears the accumulator and the reading. */
    void reset()
    {
        std::fill (sq.begin(), sq.end(), 0.0f);
        writePos = 0;
        valid = 0;
        curWindowLen = 0;
        runningSum = 0.0;
        rmsDbFs.store (-120.0f);
    }

    /** Forces the reported level to silence without touching the accumulator
        (so metering can resume from where it paused). Audio-thread safe. */
    void clearReading() noexcept       { rmsDbFs.store (-120.0f); }

    void setWindowSeconds (float s)    { windowSec.store (fxme::jlimit (0.005f, maxWindowSec, s)); }
    float getWindowSeconds() const noexcept { return windowSec.load(); }

    /** Audio thread. Feeds n samples and updates the RMS reading. `mic` may be
        null (treated as silence). */
    void process (const float* mic, int n) noexcept
    {
        const int L = fxme::jlimit (1, ringCap, (int) (windowSec.load() * sr));
        if (L != curWindowLen)
        {
            // Window length changed: restart the accumulation cleanly.
            std::fill (sq.begin(), sq.end(), 0.0f);
            writePos = 0;
            valid = 0;
            runningSum = 0.0;
            curWindowLen = L;
        }

        for (int i = 0; i < n; ++i)
        {
            const float s = mic != nullptr ? mic[i] : 0.0f;
            const float sqv = s * s;

            // Once the window is full, drop the sample leaving it (L samples ago).
            if (valid >= L)
                runningSum -= sq[(size_t) ((writePos - L + ringCap) % ringCap)];

            sq[(size_t) writePos] = sqv;
            runningSum += sqv;
            writePos = (writePos + 1) % ringCap;
            if (valid < L)
                ++valid;
        }

        const int denom = fxme::jmax (1, fxme::jmin (valid, L));
        const double meanSq = fxme::jmax (0.0, runningSum) / (double) denom;
        rmsDbFs.store (fxme::Decibels::gainToDecibels ((float) std::sqrt (meanSq), -120.0f));
    }

    float getRmsDbFs() const noexcept  { return rmsDbFs.load(); }

private:
    double sr = 44100.0;
    float  maxWindowSec = 2.0f;
    std::atomic<float> windowSec { 0.3f };
    std::atomic<float> rmsDbFs   { -120.0f };

    // Ring of squared samples with a running sum.
    std::vector<float> sq;
    int ringCap = 0, writePos = 0, valid = 0, curWindowLen = 0;
    double runningSum = 0.0;

    // Replaces JUCE's non-copyable-with-leak-detector macro; the leak detector
    // is a framework debugging aid and gets no core equivalent.
    RmsMeter (const RmsMeter&) = delete;
    RmsMeter& operator= (const RmsMeter&) = delete;
};

} // namespace fxme
