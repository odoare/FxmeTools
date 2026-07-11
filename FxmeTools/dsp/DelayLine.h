/*
  ------------------------------------------------------------------------------
    DelayLine.h

    Mono feedback delay with linear-interpolated fractional read and a
    one-pole smoothed delay-time target so time changes glide instead of
    clicking. Use one instance per channel.

    Threading: prepare() allocates — call it from prepareToPlay() only.
    Everything else is realtime-safe. Header-only, <cmath>/<vector> only.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <cmath>
#include <vector>
#include <algorithm>

namespace fxme
{

class DelayLine
{
public:
    /** Allocates the internal buffer — message thread / prepareToPlay only. */
    void prepare (double sampleRate, float maxDelaySeconds = 2.0f)
    {
        sr = sampleRate;
        size = std::max (4, (int) std::ceil (maxDelaySeconds * sampleRate) + 2);
        buffer.assign ((size_t) size, 0.0f);
        // ~30 ms smoothing of the delay-time target.
        smoothCoef = (float) std::exp (-1.0 / (0.030 * sampleRate));
        reset();
    }

    void reset()
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        writePos = 0;
        currentDelay = targetDelay;
    }

    void setDelaySeconds (float seconds)
    {
        targetDelay = std::clamp (seconds * (float) sr, 1.0f, (float) (size - 2));
    }

    void setFeedback (float fb)
    {
        feedback = std::clamp (fb, 0.0f, 0.98f);
    }

    float processSample (float x)
    {
        currentDelay = smoothCoef * currentDelay + (1.0f - smoothCoef) * targetDelay;

        float readPos = (float) writePos - currentDelay;
        if (readPos < 0.0f)
            readPos += (float) size;
        const int   i0   = (int) readPos;
        const int   i1   = (i0 + 1) % size;
        const float frac = readPos - (float) i0;
        const float delayed = buffer[(size_t) i0] + frac * (buffer[(size_t) i1] - buffer[(size_t) i0]);

        buffer[(size_t) writePos] = x + feedback * delayed;
        writePos = (writePos + 1) % size;
        return delayed;
    }

private:
    std::vector<float> buffer;
    int size = 0, writePos = 0;
    double sr = 44100.0;
    float feedback = 0.0f;
    float targetDelay = 1.0f, currentDelay = 1.0f, smoothCoef = 0.0f;
};

} // namespace fxme
