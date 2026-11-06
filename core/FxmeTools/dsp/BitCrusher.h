/*
  ------------------------------------------------------------------------------
    BitCrusher.h

    Bit-depth quantizer: rounds each sample to one of the levels of a
    2^bits-level grid over [-1, 1]. Fractional bit depths are allowed so the
    level count can be automated smoothly. Stateless, header-only, <cmath>
    only; realtime-safe.

    Three quantizer laws (setMode):

      midTread   rounds to the nearest multiple of the step: 0 is a level, so
                 silence stays silent (the default, and the only law before
                 2026-11).
      midRise    rounds to the nearest odd multiple of half a step: 0 sits
                 between two levels, so even a quiet signal toggles between
                 +-half a step (a buzzier, "always on" low end).
      muLaw      mu-law companding (mu = 255, as G.711): the signal is
                 compressed, quantized mid-tread, then expanded, so the steps
                 are fine near 0 and coarse near full scale. The colour of a
                 telephone line or an early sampler at a few bits.

    Dither: processSample (x, dither) adds `dither` steps (least significant
    bits) before rounding, in the quantizer's own domain (after the
    compression in mu-law). The caller draws the noise (TPDF, say: the
    difference of two uniform draws, +-1 step).

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
    enum class Mode { midTread = 0, midRise, muLaw };

    /** Bit depth, 1..24; fractional values interpolate the level count. */
    void setBits (float bits)
    {
        bits = bits < 1.0f ? 1.0f : (bits > 24.0f ? 24.0f : bits);
        // 2^(bits-1) quantization steps per unit: bits=1 -> ±1 only.
        levels    = std::pow (2.0f, bits - 1.0f);
        invLevels = 1.0f / levels;
    }

    void setMode (Mode m) noexcept     { mode = m; }
    Mode getMode() const noexcept      { return mode; }

    /** The quantization step (in the companded domain for mu-law). */
    float getStep() const noexcept     { return invLevels; }

    float processSample (float x) const
    {
        return processSample (x, 0.0f);
    }

    /** x quantized, with `dither` steps added before rounding. */
    float processSample (float x, float dither) const
    {
        switch (mode)
        {
            case Mode::midTread:
                return std::round (x * levels + dither) * invLevels;

            case Mode::midRise:
                return (std::floor (x * levels + dither) + 0.5f) * invLevels;

            case Mode::muLaw:
            {
                // Compress (signed), quantize, expand.
                const float c = std::copysign (std::log1p (mu * std::abs (x)) * invLogOnePlusMu, x);
                const float q = std::round (c * levels + dither) * invLevels;
                return std::copysign (std::expm1 (std::abs (q) * logOnePlusMu) * (1.0f / mu), q);
            }
        }
        return x;
    }

private:
    static constexpr float mu = 255.0f;
    static constexpr float logOnePlusMu = 5.545177444479562f;   // ln (256)
    static constexpr float invLogOnePlusMu = 1.0f / logOnePlusMu;

    float levels = 8388608.0f, invLevels = 1.0f / 8388608.0f;   // 24 bits
    Mode mode = Mode::midTread;
};

} // namespace fxme
