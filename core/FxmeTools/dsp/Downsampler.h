/*
  ------------------------------------------------------------------------------
    Downsampler.h

    Sample-rate reducer (sample & hold decimator without interpolation or
    filtering — the aliasing IS the effect). The factor is the number of
    input samples each held value covers; fractional factors are allowed
    and a factor of 1 is bit-transparent, so it needs no prepare().
    Use one instance per channel (it holds state). Header-only,
    <cmath>/<algorithm> only; realtime-safe.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <cmath>
#include <algorithm>

namespace fxme
{

class Downsampler
{
public:
    /** Hold length in input samples, 1..1024; 1 = pass-through. */
    void setFactor (float factor)
    {
        inc = 1.0f / std::clamp (factor, 1.0f, 1024.0f);
    }

    void reset()
    {
        phase = 1.0f;   // sample the very first input immediately
        held  = 0.0f;
    }

    float processSample (float x)
    {
        phase += inc;
        if (phase >= 1.0f)
        {
            phase -= std::floor (phase);
            held = x;
        }
        return held;
    }

private:
    float inc = 1.0f, phase = 1.0f, held = 0.0f;
};

} // namespace fxme
