/*
  ------------------------------------------------------------------------------
    synth/WavetableBuilder.cpp

    See WavetableBuilder.h.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/synth/WavetableBuilder.h>
#include <FxmeTools/util/Math.h>
#include <algorithm>
#include <cmath>

namespace fxme
{

WavetableBuilder::WavetableBuilder()
{
    for (int order = minWorkOrder; order <= maxWorkOrder; ++order)
        workFfts.push_back (std::make_unique<RealFft> (order));

    levelBuf.assign ((std::size_t) (2 * WavetableSet::tableSize), 0.0f);
}

//==============================================================================
int WavetableBuilder::nearestRisingZeroCrossing (const WavetableSource& source, int position, int radius) noexcept
{
    const int n = source.numSamples;
    if (n < 2)
        return position;

    // Search outwards so the nearest crossing wins.
    for (int d = 0; d <= radius; ++d)
    {
        for (int sign = 0; sign < 2; ++sign)
        {
            const int i = sign == 0 ? position + d : position - d;
            if (i < 1 || i >= n)
                continue;
            if (source.mono (i - 1) < 0.0f && source.mono (i) >= 0.0f)
                return i;
            if (d == 0)
                break;
        }
    }
    return position;
}

double WavetableBuilder::detectPeriod (const WavetableSource& source, int centre, int minLag, int maxLag)
{
    constexpr int windowOrder = 13;              // 8192-sample analysis window
    constexpr int window = 1 << windowOrder;
    constexpr int fftOrder = windowOrder + 1;    // zero padded: linear, not circular, correlation
    constexpr int fftSize = 1 << fftOrder;

    if (source.numSamples < 4 * minLag)
        return 0.0;

    if (acfFft == nullptr)
    {
        acfFft = std::make_unique<RealFft> (fftOrder);
        acfBuf.assign ((std::size_t) (2 * fftSize), 0.0f);
    }

    const int n = jmin (window, source.numSamples);
    const int start = jlimit (0, source.numSamples - n, centre - n / 2);
    maxLag = jmin (maxLag, n / 2);
    if (maxLag <= minLag)
        return 0.0;

    std::fill (acfBuf.begin(), acfBuf.end(), 0.0f);
    double mean = 0.0;
    for (int i = 0; i < n; ++i)
        mean += source.mono (start + i);
    mean /= (double) n;
    for (int i = 0; i < n; ++i)
        acfBuf[(std::size_t) i] = source.mono (start + i) - (float) mean;

    acfFft->performRealOnlyForwardTransform (acfBuf.data());
    for (int k = 0; k <= fftSize / 2; ++k)
    {
        const float re = acfBuf[(std::size_t) (2 * k)], im = acfBuf[(std::size_t) (2 * k + 1)];
        acfBuf[(std::size_t) (2 * k)] = re * re + im * im;
        acfBuf[(std::size_t) (2 * k + 1)] = 0.0f;
    }
    acfFft->performRealOnlyInverseTransform (acfBuf.data());

    const float r0 = acfBuf[0];
    if (r0 <= 1.0e-12f)
        return 0.0;

    // Normalised, unbiased autocorrelation.
    auto rn = [&] (int lag)
    {
        return (double) acfBuf[(std::size_t) lag] / (double) r0 * (double) n / (double) (n - lag);
    };

    double best = -1.0;
    for (int lag = minLag; lag <= maxLag; ++lag)
        best = jmax (best, rn (lag));

    if (best < 0.3)
        return 0.0;   // no clear periodicity (noise, a transient)

    // The first peak close to the best one: avoids picking two periods
    // (an octave error) when they correlate almost as well as one.
    for (int lag = minLag + 1; lag < maxLag; ++lag)
    {
        const double v = rn (lag);
        if (v >= 0.9 * best && v >= rn (lag - 1) && v >= rn (lag + 1))
        {
            // Parabolic refinement of the peak position.
            const double a = rn (lag - 1), b = v, c = rn (lag + 1);
            const double denom = a - 2.0 * b + c;
            const double shift = std::abs (denom) > 1.0e-12 ? 0.5 * (a - c) / denom : 0.0;
            return (double) lag + jlimit (-0.5, 0.5, shift);
        }
    }
    return 0.0;
}

//==============================================================================
void WavetableBuilder::resolveRange (const WavetableSource& source, const WavetableBuildSettings& s,
                                     int& start, int& length)
{
    const int n = source.numSamples;
    length = jlimit (minWidth, jmax (minWidth, n), roundToInt (s.width));
    length = jmin (length, n);
    start = jlimit (0, jmax (0, n - length), roundToInt (s.centre - 0.5 * (double) length));

    if (s.snap == WavetableBuildSettings::Snap::zeroCrossing)
    {
        const int radius = jmin (2048, jmax (1, length / 4));
        const int a = nearestRisingZeroCrossing (source, start, radius);
        const int b = nearestRisingZeroCrossing (source, start + length, radius);
        if (b - a >= minWidth && b <= n)
        {
            start = a;
            length = b - a;
        }
    }
    else if (s.snap == WavetableBuildSettings::Snap::period)
    {
        const double period = detectPeriod (source, roundToInt (s.centre));
        if (period > 0.0)
        {
            const double periods = jmax (1.0, std::round ((double) length / period));
            length = jlimit (minWidth, n, roundToInt (periods * period));
            const int wanted = roundToInt (s.centre - 0.5 * (double) length);
            start = nearestRisingZeroCrossing (source, jlimit (0, jmax (0, n - length), wanted),
                                               jmax (1, roundToInt (period * 0.5)));
            start = jlimit (0, jmax (0, n - length), start);
        }
    }
}

void WavetableBuilder::makeCycle (const float* src, int sourceLength, int start, int length, int overlapSamples,
                                  WavetableBuildSettings::OverlapShape shape)
{
    (void) sourceLength;

    const int xf = jlimit (0, length / 2, overlapSamples);
    cycleLength = length - xf;
    if ((int) cycle.size() < cycleLength)
        cycle.resize ((std::size_t) cycleLength);

    for (int i = 0; i < cycleLength; ++i)
        cycle[(std::size_t) i] = src[start + i];

    // The tail [cycleLength, length) fades out over the head [0, xf), which
    // fades in: at i = 0 the cycle is the sample right after its own end, so
    // the wrap is continuous, and at i = xf it is the source again.
    for (int i = 0; i < xf; ++i)
    {
        const float t = ((float) i + 0.5f) / (float) xf;
        float gIn, gOut;
        if (shape == WavetableBuildSettings::OverlapShape::equalPower)
        {
            gIn  = std::sin (0.5f * MathConstants<float>::pi * t);
            gOut = std::cos (0.5f * MathConstants<float>::pi * t);
        }
        else
        {
            gIn = t;
            gOut = 1.0f - t;
        }
        cycle[(std::size_t) i] = gIn * src[start + i] + gOut * src[start + cycleLength + i];
    }
}

void WavetableBuilder::resampleCycle (int workSize)
{
    float* w = work.data();
    const int len = cycleLength;

    if (len <= workSize)
    {
        // Upsampling: periodic cubic Hermite.
        auto at = [&] (int i)
        {
            i %= len;
            if (i < 0)
                i += len;
            return cycle[(std::size_t) i];
        };

        const double step = (double) len / (double) workSize;
        for (int j = 0; j < workSize; ++j)
        {
            const double pos = (double) j * step;
            const int i = (int) pos;
            const float f = (float) (pos - (double) i);
            const float y0 = at (i - 1), y1 = at (i), y2 = at (i + 1), y3 = at (i + 2);
            const float c1 = 0.5f * (y2 - y0);
            const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
            const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
            w[j] = ((c3 * f + c2) * f + c1) * f + y1;
        }
    }
    else
    {
        // Decimation: average every source sample into its bin. The second
        // half of the work buffer holds the counts.
        float* counts = w + workSize;
        std::fill (w, w + 2 * workSize, 0.0f);
        for (int i = 0; i < len; ++i)
        {
            const int b = (int) (((std::int64_t) i * workSize) / len);
            w[b] += cycle[(std::size_t) i];
            counts[b] += 1.0f;
        }
        for (int j = 0; j < workSize; ++j)
            w[j] = counts[j] > 0.0f ? w[j] / counts[j] : 0.0f;
    }

    std::fill (w + workSize, w + 2 * workSize, 0.0f);
}

void WavetableBuilder::buildLevels (int workOrder, int harmonicLimit, float* const* tables)
{
    const int workSize = 1 << workOrder;
    workFfts[(std::size_t) (workOrder - minWorkOrder)]->performRealOnlyForwardTransform (work.data());

    const float scale = (float) WavetableSet::tableSize / (float) workSize;
    const int maxBin = jmin (workSize / 2, WavetableSet::tableSize / 2 - 1);

    for (int level = 0; level < WavetableSet::numLevels; ++level)
    {
        std::fill (levelBuf.begin(), levelBuf.end(), 0.0f);
        const int top = jmin (WavetableSet::maxHarmonic (level), harmonicLimit, maxBin);
        for (int h = 1; h <= top; ++h)    // bin 0 (DC) stays at zero
        {
            levelBuf[(std::size_t) (2 * h)]     = work[(std::size_t) (2 * h)]     * scale;
            levelBuf[(std::size_t) (2 * h + 1)] = work[(std::size_t) (2 * h + 1)] * scale;
        }
        tableFft.performRealOnlyInverseTransform (levelBuf.data());
        std::copy (levelBuf.begin(), levelBuf.begin() + WavetableSet::tableSize, tables[level]);
    }
}

//==============================================================================
bool WavetableBuilder::build (const WavetableSource& source, const WavetableBuildSettings& settings,
                              WavetableSet& out)
{
    if (! out.isAllocated())
        out.allocate();

    const int numChannels = jlimit (1, WavetableSet::maxChannels, source.getNumChannels());
    out.numChannels = numChannels;

    if (source.numSamples < minWidth || source.channels.empty())
    {
        std::fill (out.data.begin(), out.data.end(), 0.0f);
        lastStart = lastLength = 0;
        return false;
    }

    int start = 0, length = 0;
    resolveRange (source, settings, start, length);
    lastStart = start;
    lastLength = length;

    const int overlapSamples = roundToInt (jlimit (0.0f, 0.5f, settings.overlap) * (float) length);
    const int expectedCycle = length - jlimit (0, length / 2, overlapSamples);
    const int workSize = jlimit (1 << minWorkOrder, 1 << maxWorkOrder, nextPowerOfTwo (expectedCycle));
    int workOrder = minWorkOrder;
    while ((1 << workOrder) < workSize)
        ++workOrder;

    if ((int) work.size() < 2 * workSize)
        work.resize ((std::size_t) (2 * workSize));

    for (int ch = 0; ch < numChannels; ++ch)
    {
        makeCycle (source.channels[(std::size_t) ch].data(), source.numSamples, start, length,
                   overlapSamples, settings.overlapShape);
        resampleCycle (workSize);

        // Upsampled: the cycle holds no harmonic at or above half its length.
        const int harmonicLimit = cycleLength <= workSize ? jmax (1, (cycleLength - 1) / 2)
                                                          : workSize / 2 - 1;

        float* tables[WavetableSet::numLevels];
        for (int level = 0; level < WavetableSet::numLevels; ++level)
            tables[level] = out.table (ch, level);
        buildLevels (workOrder, harmonicLimit, tables);

        for (int level = 0; level < WavetableSet::numLevels; ++level)
            out.fillGuards (ch, level);
    }
    return true;
}

} // namespace fxme
