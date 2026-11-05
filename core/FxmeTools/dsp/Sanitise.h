/*
  ------------------------------------------------------------------------------
    Sanitise.h

    Keeps bad samples out of a recursive path. A NaN or an infinity that
    reaches a feedback loop, a delay line fed back into itself or an IIR
    filter's state stays there for good (0 x NaN is still NaN, and Inf - Inf
    is NaN), and the effect outputs NaN from then on; one stray sample from
    upstream is enough. A clamp does not help: comparisons with a NaN are all
    false, so a jlimit or std::clamp lets it through.

      sanitiseSample (x, limit)   a finite x, clamped to [-limit, limit];
                                  NaN and infinities become 0. For what
                                  enters the loop.
      isOutOfBounds (x, limit)    true when |x| > limit or x is not finite.
                                  For a state that went bad anyway (an
                                  unstable filter, say): the caller clears it.
      sanitiseBlock (data, numChannels, numSamples, limit)
                                  sanitiseSample on a whole block, in place.
                                  For an effect whose main path itself holds
                                  state (filters, detectors, an FFT): cleaning
                                  its input is then the whole fix. With a
                                  generous limit (kMainPathLimit, +60 dBFS)
                                  only NaN and infinities change.

    Only the recursive path needs this: a bad sample on a dry path passes
    once and is gone. No fast-math assumption: std::isfinite is reliable as
    long as the build does not pass -ffinite-math-only (or -ffast-math).

    Header-only, <cmath> and <algorithm> only, realtime-safe.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <algorithm>
#include <cmath>

namespace fxme
{

/** x clamped to [-limit, limit], or 0 when x is NaN or infinite. */
inline float sanitiseSample (float x, float limit) noexcept
{
    return std::isfinite (x) ? std::clamp (x, -limit, limit) : 0.0f;
}

/** True when x is NaN, infinite, or beyond +-limit. */
inline bool isOutOfBounds (float x, float limit) noexcept
{
    return ! (std::abs (x) <= limit);   // false for a NaN, so the negation catches it
}

/** A limit for a main signal path: +60 dBFS, far above anything real, so
    only NaN and infinities are changed. */
inline constexpr float kMainPathLimit = 1000.0f;

/** sanitiseSample on every sample of a block, in place. */
inline void sanitiseBlock (float* const* data, int numChannels, int numSamples, float limit) noexcept
{
    for (int ch = 0; ch < numChannels; ++ch)
    {
        float* x = data[ch];
        for (int i = 0; i < numSamples; ++i)
            x[i] = sanitiseSample (x[i], limit);
    }
}

} // namespace fxme
