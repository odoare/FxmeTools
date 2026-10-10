/*
  ------------------------------------------------------------------------------
    synth/SourceAnalysis.cpp

    See SourceAnalysis.h.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/synth/SourceAnalysis.h>
#include <FxmeTools/util/Fft.h>
#include <FxmeTools/util/Math.h>
#include <algorithm>
#include <cmath>

namespace fxme
{

void SourceAnalysis::build (const float* const* channels, int numChannels, int numSamples)
{
    zeroCrossings.clear();
    periods.clear();
    if (numChannels <= 0 || numSamples < 2)
        return;

    std::vector<float> mono ((std::size_t) numSamples, 0.0f);
    for (int ch = 0; ch < numChannels; ++ch)
        for (int i = 0; i < numSamples; ++i)
            mono[(std::size_t) i] += channels[ch][i] / (float) numChannels;

    for (int i = 1; i < numSamples; ++i)
        if (mono[(std::size_t) i - 1] < 0.0f && mono[(std::size_t) i] >= 0.0f)
            zeroCrossings.push_back (i);

    // Period track.
    constexpr int window = 2 * maxLag;          // 4096
    constexpr int fftOrder = 13;                // 8192: zero padded, linear correlation
    constexpr int fftSize = 1 << fftOrder;
    RealFft fft (fftOrder);
    std::vector<float> buf ((std::size_t) (2 * fftSize));

    const int frames = numSamples / hop + 1;
    periods.assign ((std::size_t) frames, 0.0f);
    for (int f = 0; f < frames; ++f)
    {
        const int n = jmin (window, numSamples);
        const int start = jlimit (0, numSamples - n, f * hop - n / 2);
        const int top = jmin (maxLag, n / 2);
        if (top <= minLag)
            continue;

        std::fill (buf.begin(), buf.end(), 0.0f);
        double mean = 0.0;
        for (int i = 0; i < n; ++i)
            mean += mono[(std::size_t) (start + i)];
        mean /= (double) n;
        for (int i = 0; i < n; ++i)
            buf[(std::size_t) i] = mono[(std::size_t) (start + i)] - (float) mean;

        fft.performRealOnlyForwardTransform (buf.data());
        for (int k = 0; k <= fftSize / 2; ++k)
        {
            const float re = buf[(std::size_t) (2 * k)], im = buf[(std::size_t) (2 * k + 1)];
            buf[(std::size_t) (2 * k)] = re * re + im * im;
            buf[(std::size_t) (2 * k + 1)] = 0.0f;
        }
        fft.performRealOnlyInverseTransform (buf.data());

        const double r0 = buf[0];
        if (r0 <= 1.0e-12)
            continue;
        auto rn = [&] (int lag) { return (double) buf[(std::size_t) lag] / r0 * (double) n / (double) (n - lag); };

        double best = -1.0;
        for (int lag = minLag; lag <= top; ++lag)
            best = jmax (best, rn (lag));
        if (best < 0.3)
            continue;

        for (int lag = minLag + 1; lag < top; ++lag)
        {
            const double v = rn (lag);
            if (v >= 0.9 * best && v >= rn (lag - 1) && v >= rn (lag + 1))
            {
                const double a = rn (lag - 1), c = rn (lag + 1);
                const double denom = a - 2.0 * v + c;
                const double shift = std::abs (denom) > 1.0e-12 ? 0.5 * (a - c) / denom : 0.0;
                periods[(std::size_t) f] = (float) ((double) lag + jlimit (-0.5, 0.5, shift));
                break;
            }
        }
    }
}

double SourceAnalysis::nearestRisingZeroCrossing (double position, double radius) const noexcept
{
    if (zeroCrossings.empty())
        return position;
    const auto it = std::lower_bound (zeroCrossings.begin(), zeroCrossings.end(), (int) std::ceil (position));
    double best = position, bestDistance = radius + 1.0;
    if (it != zeroCrossings.end() && (double) *it - position < bestDistance)
    {
        best = (double) *it;
        bestDistance = (double) *it - position;
    }
    if (it != zeroCrossings.begin() && position - (double) *(it - 1) < bestDistance)
    {
        best = (double) *(it - 1);
        bestDistance = position - (double) *(it - 1);
    }
    return bestDistance <= radius ? best : position;
}

double SourceAnalysis::periodAt (double position) const noexcept
{
    if (periods.empty())
        return 0.0;
    const double f = position / (double) hop;
    const int i = jlimit (0, (int) periods.size() - 1, (int) std::floor (f));
    const int j = jmin ((int) periods.size() - 1, i + 1);
    const double a = periods[(std::size_t) i], b = periods[(std::size_t) j];
    if (a <= 0.0 || b <= 0.0)
        return jmax (a, b);   // no clear period on one side: take the other, or none
    const double t = jlimit (0.0, 1.0, f - (double) i);
    return a + (b - a) * t;
}

} // namespace fxme
