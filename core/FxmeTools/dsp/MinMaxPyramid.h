/*
  ------------------------------------------------------------------------------
    dsp/MinMaxPyramid.h

    Min / max overview of a long signal, for drawing a waveform at any zoom
    without scanning every sample on each repaint. Level 0 holds the min and
    max of every `baseBucket` samples, each next level halves the count.
    rangeAt() answers "min and max over samples [a, b)" from the coarsest
    level whose buckets are still small enough, then only scans the edges.

    Built once (allocates) from a finished signal, on any thread; read-only
    afterwards, so it can be shared freely.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <algorithm>
#include <vector>

namespace fxme
{

class MinMaxPyramid
{
public:
    static constexpr int baseBucket = 64;

    void build (const float* data, int numSamples)
    {
        source = data;
        length = numSamples;
        levels.clear();
        if (numSamples <= 0)
            return;

        Level base;
        const int n = (numSamples + baseBucket - 1) / baseBucket;
        base.mins.resize ((std::size_t) n);
        base.maxs.resize ((std::size_t) n);
        for (int b = 0; b < n; ++b)
        {
            const int s0 = b * baseBucket, s1 = std::min (numSamples, s0 + baseBucket);
            float lo = data[s0], hi = data[s0];
            for (int i = s0 + 1; i < s1; ++i)
            {
                lo = std::min (lo, data[i]);
                hi = std::max (hi, data[i]);
            }
            base.mins[(std::size_t) b] = lo;
            base.maxs[(std::size_t) b] = hi;
        }
        levels.push_back (std::move (base));

        while (levels.back().mins.size() > 1)
        {
            const auto& prev = levels.back();
            Level next;
            const std::size_t m = (prev.mins.size() + 1) / 2;
            next.mins.resize (m);
            next.maxs.resize (m);
            for (std::size_t b = 0; b < m; ++b)
            {
                const std::size_t a = 2 * b, c = std::min (prev.mins.size() - 1, 2 * b + 1);
                next.mins[b] = std::min (prev.mins[a], prev.mins[c]);
                next.maxs[b] = std::max (prev.maxs[a], prev.maxs[c]);
            }
            levels.push_back (std::move (next));
        }
    }

    int getLength() const noexcept { return length; }

    /** Min and max over samples [a, b). The source buffer passed to build()
        must still be alive. */
    void rangeAt (int a, int b, float& lo, float& hi) const noexcept
    {
        a = std::max (0, a);
        b = std::min (length, b);
        if (b <= a || source == nullptr)
        {
            lo = hi = 0.0f;
            return;
        }

        // Small span: scan the samples directly.
        if (b - a <= 2 * baseBucket || levels.empty())
        {
            lo = hi = source[a];
            for (int i = a + 1; i < b; ++i)
            {
                lo = std::min (lo, source[i]);
                hi = std::max (hi, source[i]);
            }
            return;
        }

        // The coarsest level with at least two buckets inside the span.
        int level = 0;
        int bucket = baseBucket;
        while (level + 1 < (int) levels.size() && (b - a) >= 4 * bucket)
        {
            ++level;
            bucket *= 2;
        }

        const auto& L = levels[(std::size_t) level];
        const int first = (a + bucket - 1) / bucket, last = b / bucket;   // whole buckets [first, last)
        lo = source[a];
        hi = source[a];
        for (int i = a; i < std::min (b, first * bucket); ++i)
        {
            lo = std::min (lo, source[i]);
            hi = std::max (hi, source[i]);
        }
        for (int k = first; k < last && k < (int) L.mins.size(); ++k)
        {
            lo = std::min (lo, L.mins[(std::size_t) k]);
            hi = std::max (hi, L.maxs[(std::size_t) k]);
        }
        for (int i = std::max (a, last * bucket); i < b; ++i)
        {
            lo = std::min (lo, source[i]);
            hi = std::max (hi, source[i]);
        }
    }

private:
    struct Level
    {
        std::vector<float> mins, maxs;
    };

    const float* source = nullptr;
    int length = 0;
    std::vector<Level> levels;
};

} // namespace fxme
