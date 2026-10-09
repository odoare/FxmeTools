/*
  ------------------------------------------------------------------------------
    synth/WavetableReader.h

    Reads a WavetableSet at a fractional phase, with three interpolation
    qualities:

      linear   2 taps. Fine on the upper mip levels, which are heavily
               oversampled (level 4 holds 64 harmonics in 2048 samples).
      cubic    4-tap Hermite. The default: clean on every level.
      sinc     8-tap Blackman-Harris windowed sinc over a 512-phase kernel.
               The flattest passband, for the lowest levels of bright tables.

    Stateless: the phase, the mip level and any crossfade between levels or
    sets belong to the caller (a voice). Realtime safe; call warmUp() once
    from a non-audio thread so the sinc kernel is computed there rather than
    on the first sinc read.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/synth/WavetableSet.h>
#include <array>
#include <cmath>

namespace fxme
{

class WavetableReader
{
public:
    enum class Interpolation : int { linear = 0, cubic, sinc };

    static constexpr int sincTaps = 8;
    static constexpr int sincPhases = 512;

    /** One table at phase in [0, 1). */
    static float read (const float* table, double phase, Interpolation mode) noexcept
    {
        const double pos = phase * (double) WavetableSet::tableSize;
        int i = (int) pos;
        const float f = (float) (pos - (double) i);
        i &= WavetableSet::tableSize - 1;

        switch (mode)
        {
            case Interpolation::linear:
                return table[i] + f * (table[i + 1] - table[i]);

            case Interpolation::sinc:
            {
                const auto& k = kernel();
                const int p = (int) (f * (float) sincPhases);
                const float* c = k.data() + p * sincTaps;
                const float* x = table + i - (sincTaps / 2 - 1);
                float acc = 0.0f;
                for (int t = 0; t < sincTaps; ++t)
                    acc += c[t] * x[t];
                return acc;
            }

            case Interpolation::cubic:
            default:
            {
                const float y0 = table[i - 1], y1 = table[i], y2 = table[i + 1], y3 = table[i + 2];
                const float c1 = 0.5f * (y2 - y0);
                const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
                const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
                return ((c3 * f + c2) * f + c1) * f + y1;
            }
        }
    }

    /** Both channels of one level (a mono set gives the same value twice). */
    static void readStereo (const WavetableSet& set, int level, double phase, Interpolation mode,
                            float& left, float& right) noexcept
    {
        left = read (set.table (0, level), phase, mode);
        right = set.numChannels > 1 ? read (set.table (1, level), phase, mode) : left;
    }

    /** Computes the sinc kernel now (it is otherwise made on first use). */
    static void warmUp() noexcept { (void) kernel(); }

private:
    using Kernel = std::array<float, (sincPhases + 1) * sincTaps>;

    static const Kernel& kernel() noexcept
    {
        static const Kernel k = []
        {
            Kernel table {};
            constexpr double pi = 3.14159265358979323846;
            for (int p = 0; p <= sincPhases; ++p)
            {
                const double frac = (double) p / (double) sincPhases;
                double sum = 0.0;
                double taps[sincTaps];
                for (int t = 0; t < sincTaps; ++t)
                {
                    // Tap t sits at offset (t - 3) from the integer position.
                    const double x = (double) (t - (sincTaps / 2 - 1)) - frac;
                    const double s = std::abs (x) < 1.0e-9 ? 1.0 : std::sin (pi * x) / (pi * x);
                    // Blackman-Harris window over [-4, 4].
                    const double w = (x + (double) sincTaps * 0.5) / (double) sincTaps;
                    const double win = 0.35875 - 0.48829 * std::cos (2.0 * pi * w)
                                     + 0.14128 * std::cos (4.0 * pi * w) - 0.01168 * std::cos (6.0 * pi * w);
                    taps[t] = s * win;
                    sum += taps[t];
                }
                for (int t = 0; t < sincTaps; ++t)
                    table[(std::size_t) (p * sincTaps + t)] = (float) (taps[t] / sum);   // unity DC gain
            }
            return table;
        }();
        return k;
    }
};

} // namespace fxme
