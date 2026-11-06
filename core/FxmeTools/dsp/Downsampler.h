/*
  ------------------------------------------------------------------------------
    Downsampler.h

    Sample-rate reducer (sample & hold decimator without interpolation or
    filtering — the aliasing IS the effect). The factor is the number of
    input samples each held value covers; fractional factors are allowed
    and a factor of 1 is bit-transparent, so it needs no prepare().
    Use one instance per channel (it holds state). Header-only,
    <cmath>/<algorithm>/<cstdint> only; realtime-safe.

    Jitter (setJitter, 0 by default): each hold's length is the factor times
    a random amount within +-jitter (0..1), drawn anew at each latch. That
    blurs the fixed aliasing pattern of a steady rate into something closer
    to noise. The draws are deterministic (fxme::detrand on the seed and a
    latch counter that reset() rewinds), so instances on several channels
    with the same seed and the same factor hold in lockstep, and a bounce
    renders the same every time.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/dsp/DeterministicRandom.h>
#include <cmath>
#include <algorithm>
#include <cstdint>

namespace fxme
{

class Downsampler
{
public:
    /** Hold length in input samples, 1..1024; 1 = pass-through. */
    void setFactor (float factor)
    {
        baseFactor = std::clamp (factor, 1.0f, 1024.0f);
        if (jitter <= 0.0f)
            inc = 1.0f / baseFactor;
    }

    /** Random spread of each hold's length, 0..1 of the factor (0: none). */
    void setJitter (float amount, std::uint64_t newSeed = 0) noexcept
    {
        jitter = std::clamp (amount, 0.0f, 1.0f);
        seed = newSeed;
        if (jitter <= 0.0f)
            inc = 1.0f / baseFactor;
    }

    void reset()
    {
        phase = 1.0f;   // sample the very first input immediately
        held  = 0.0f;
        latchCount = 0;
    }

    float processSample (float x)
    {
        // Latch first, advance after. The other order costs a sample: with
        // phase starting at 1 so the first input is taken immediately,
        // incrementing first leaves phase at `inc` rather than 0 after that
        // latch, so the opening group runs one sample short and every later
        // latch lands on the last sample of its group instead of the first.
        if (phase >= 1.0f)
        {
            phase -= std::floor (phase);
            held = x;
            if (jitter > 0.0f)
            {
                const float u = detrand::u01 (seed, latchCount++);
                inc = 1.0f / std::max (1.0f, baseFactor * (1.0f + jitter * (2.0f * u - 1.0f)));
            }
        }
        phase += inc;
        return held;
    }

private:
    float inc = 1.0f, phase = 1.0f, held = 0.0f;
    float baseFactor = 1.0f, jitter = 0.0f;
    std::uint64_t seed = 0, latchCount = 0;
};

} // namespace fxme
