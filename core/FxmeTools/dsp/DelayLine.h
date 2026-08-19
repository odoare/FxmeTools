/*
  ------------------------------------------------------------------------------
    DelayLine.h

    Mono feedback delay with linear-interpolated fractional read and a
    one-pole smoothed delay-time target so time changes glide instead of
    clicking (the glide time is settable — long values give an audible
    portamento when the delay is short enough to be pitched). An optional
    one-pole lowpass in the feedback path damps the repeats like a
    Karplus-Strong string. Use one instance per channel.

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
        setSmoothingSeconds (smoothSeconds);
        setDamping (damping);
        reset();
    }

    void reset()
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        writePos = 0;
        currentDelay = targetDelay;
        lpState = 0.0f;
    }

    /** Glide time of the delay-time target (default 30 ms). Short values
        make time changes snappy; long values give a tape-style pitch slur. */
    void setSmoothingSeconds (float seconds)
    {
        smoothSeconds = std::clamp (seconds, 0.0005f, 0.5f);
        smoothCoef = (float) std::exp (-1.0 / (smoothSeconds * sr));
    }

    /** 0 = no damping (feedback path is bit-transparent), 1 = strong
        damping. Maps exponentially to a one-pole lowpass cutoff from
        ~20 kHz down to ~200 Hz, so the repeats mellow as they decay. */
    void setDamping (float damp01)
    {
        damping = std::clamp (damp01, 0.0f, 1.0f);
        if (damping < 1.0e-4f)
        {
            lpCoef = 1.0f;   // exact pass-through
            return;
        }
        const double cutoff = 20000.0 * std::pow (0.01, (double) damping);
        lpCoef = 1.0f - (float) std::exp (-6.283185307179586 * cutoff / sr);
    }

    void setDelaySeconds (float seconds)
    {
        targetDelay = std::clamp (seconds * (float) sr, 1.0f, (float) (size - 2));
    }

    void setFeedback (float fb)
    {
        feedback = std::clamp (fb, 0.0f, 0.999f);
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

        lpState += lpCoef * (delayed - lpState);
        buffer[(size_t) writePos] = x + feedback * lpState;
        writePos = (writePos + 1) % size;
        return delayed;
    }

private:
    std::vector<float> buffer;
    int size = 0, writePos = 0;
    double sr = 44100.0;
    float feedback = 0.0f;
    float targetDelay = 1.0f, currentDelay = 1.0f;
    float smoothSeconds = 0.030f, smoothCoef = 0.0f;
    float damping = 0.0f, lpCoef = 1.0f, lpState = 0.0f;
};

} // namespace fxme
