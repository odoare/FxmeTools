/*
  ------------------------------------------------------------------------------
    BitCrusher.h

    Bit-depth quantizer: rounds each sample to the nearest of 2^bits levels
    over [-1, 1]. Fractional bit depths are allowed so the level count can be
    automated smoothly. Stateless, header-only, <cmath> only; realtime-safe.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <cmath>

namespace fxme
{

class BitCrusher
{
public:
    /** Bit depth, 1..24; fractional values interpolate the level count. */
    void setBits (float bits)
    {
        bits = bits < 1.0f ? 1.0f : (bits > 24.0f ? 24.0f : bits);
        // 2^(bits-1) quantization steps per unit: bits=1 -> ±1 only.
        levels    = std::pow (2.0f, bits - 1.0f);
        invLevels = 1.0f / levels;
    }

    float processSample (float x) const
    {
        return std::round (x * levels) * invLevels;
    }

private:
    float levels = 8388608.0f, invLevels = 1.0f / 8388608.0f;   // 24 bits
};

} // namespace fxme
