/*
  ------------------------------------------------------------------------------
    ModDelayLine.h

    Mono delay line meant to be *modulated*: the read position is given at
    every sample and interpolated with a 4-point Catmull-Rom kernel, so a
    swept delay glides without the dulling and zipper noise that linear
    interpolation produces on a chorus or flanger.

    Unlike fxme::DelayLine this line has no smoothing of its own and no
    built-in feedback: read and write are separate calls, which is what lets
    a flanger read first, mix the feedback in, and write the result. Use one
    instance per channel.

      line.write (x + feedback * line.read (delaySamples));

    Threading: prepare() allocates — call it from prepareToPlay() only.
    Everything else is realtime-safe. Header-only, <cmath>/<vector> only.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace fxme
{

class ModDelayLine
{
public:
    /** Allocates the internal buffer — message thread / prepareToPlay only. */
    void prepare (double sampleRate, float maxDelaySeconds)
    {
        sr   = sampleRate > 0.0 ? sampleRate : 44100.0;
        size = std::max (8, (int) std::ceil (maxDelaySeconds * sr) + 4);
        buffer.assign ((size_t) size, 0.0f);
        reset();
    }

    void reset()
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        writePos = 0;
    }

    double getSampleRate() const noexcept { return sr; }

    /** Longest delay the line can serve, in samples. */
    float maxDelaySamples() const noexcept { return (float) (size - 3); }

    /** Reads `delaySamples` back from the write position with Catmull-Rom
        interpolation. The request is clamped to [2, size-3]: the kernel needs
        one sample either side of the fractional position, and the slot at the
        write pointer still holds the oldest sample. */
    float read (float delaySamples) const noexcept
    {
        const float d = std::clamp (delaySamples, 2.0f, (float) (size - 3));

        float rp = (float) writePos - d;
        if (rp < 0.0f)
            rp += (float) size;

        const int   i1   = (int) rp;
        const float frac = rp - (float) i1;
        const int   i0   = (i1 + size - 1) % size;
        const int   i2   = (i1 + 1) % size;
        const int   i3   = (i1 + 2) % size;

        const float y0 = buffer[(size_t) i0];
        const float y1 = buffer[(size_t) i1];
        const float y2 = buffer[(size_t) i2];
        const float y3 = buffer[(size_t) i3];

        const float c0 = y1;
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);

        return ((c3 * frac + c2) * frac + c1) * frac + c0;
    }

    /** Writes one sample and advances the write position. */
    void write (float x) noexcept
    {
        buffer[(size_t) writePos] = x;
        writePos = (writePos + 1) % size;
    }

private:
    std::vector<float> buffer;
    int    size     = 0;
    int    writePos = 0;
    double sr       = 44100.0;
};

} // namespace fxme
