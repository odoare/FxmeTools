/*
  ------------------------------------------------------------------------------
    synth/MipmappedBuffer.cpp

    See MipmappedBuffer.h.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/synth/MipmappedBuffer.h>
#include <FxmeTools/util/Math.h>
#include <algorithm>
#include <array>

namespace fxme
{

namespace
{
    constexpr int halfTaps = 31;             // 63-tap decimation filter
    constexpr int numTaps = 2 * halfTaps + 1;

    /** Windowed sinc low-pass at 0.23 cycles per input sample (just under the
        decimated Nyquist), Blackman-Harris window, unity DC gain. */
    const std::array<float, numTaps>& decimationFilter()
    {
        static const std::array<float, numTaps> h = []
        {
            std::array<float, numTaps> taps {};
            constexpr double fc = 0.23, pi = 3.14159265358979323846;
            double sum = 0.0;
            for (int j = 0; j < numTaps; ++j)
            {
                const double n = (double) (j - halfTaps);
                const double s = n == 0.0 ? 2.0 * fc : std::sin (2.0 * pi * fc * n) / (pi * n);
                const double w = (double) j / (double) (numTaps - 1);
                const double win = 0.35875 - 0.48829 * std::cos (2.0 * pi * w)
                                 + 0.14128 * std::cos (4.0 * pi * w) - 0.01168 * std::cos (6.0 * pi * w);
                taps[(std::size_t) j] = (float) (s * win);
                sum += s * win;
            }
            for (auto& t : taps)
                t = (float) ((double) t / sum);
            return taps;
        }();
        return h;
    }
}

void MipmappedBuffer::build (const float* const* channels, int newNumChannels, int newNumSamples)
{
    numChannels = jmax (0, newNumChannels);
    numSamples = jmax (0, newNumSamples);
    levels.assign ((std::size_t) numChannels, {});
    levelScale.clear();
    numLevels = 0;
    if (numChannels == 0 || numSamples == 0)
        return;

    // How many levels: halve until a level would be shorter than minLevelLength.
    int count = 1;
    for (int n = numSamples; count < maxLevels && (n + 1) / 2 >= minLevelLength; n = (n + 1) / 2)
        ++count;
    numLevels = count;
    for (int k = 0; k < numLevels; ++k)
        levelScale.push_back (1.0 / (double) (1 << k));

    const auto& h = decimationFilter();

    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto& chLevels = levels[(std::size_t) ch];
        chLevels.resize ((std::size_t) numLevels);

        auto pad = [] (Level& lv)
        {
            float* d = lv.data.data();
            for (int g = 0; g < guard; ++g)
            {
                d[g] = d[guard];
                d[guard + lv.length + g] = d[guard + lv.length - 1];
            }
        };

        auto& l0 = chLevels[0];
        l0.length = numSamples;
        l0.data.assign ((std::size_t) (numSamples + 2 * guard), 0.0f);
        std::copy (channels[ch], channels[ch] + numSamples, l0.data.begin() + guard);
        pad (l0);

        for (int k = 1; k < numLevels; ++k)
        {
            const auto& src = chLevels[(std::size_t) k - 1];
            auto& dst = chLevels[(std::size_t) k];
            dst.length = (src.length + 1) / 2;
            dst.data.assign ((std::size_t) (dst.length + 2 * guard), 0.0f);

            const float* x = src.data.data() + guard;
            const int n = src.length;
            for (int m = 0; m < dst.length; ++m)
            {
                // Centred on input sample 2m, so level k sample m sits at
                // level-0 position m * 2^k.
                double acc = 0.0;
                const int c = 2 * m;
                if (c - halfTaps >= 0 && c + halfTaps < n)
                {
                    const float* xc = x + c - halfTaps;
                    for (int j = 0; j < numTaps; ++j)
                        acc += (double) h[(std::size_t) j] * (double) xc[j];
                }
                else
                    for (int j = -halfTaps; j <= halfTaps; ++j)
                        acc += (double) h[(std::size_t) (j + halfTaps)] * (double) x[jlimit (0, n - 1, c + j)];
                dst.data[(std::size_t) (guard + m)] = (float) acc;
            }
            pad (dst);
        }
    }
}

} // namespace fxme
