/*
  ------------------------------------------------------------------------------
    DeterministicRandom.h

    Stateless, hash-based random draws for reproducible "random" musical
    events. Unlike a sequential RNG, every draw is a pure function of its
    inputs (seed + event coordinates), so results are identical across
    sessions, offline/realtime bounces and transport jumps: re-entering the
    same block on the same loop pass always yields the same value.

    Typical use:
        float u = fxme::detrand::u01 (seed, laneIndex, blockId, loopIndex, drawIndex);
        int   k = fxme::detrand::weightedChoice (weights, n, u);

    Header-only, <cstdint> only; realtime-safe.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <cstdint>

namespace fxme::detrand
{

/** splitmix64 avalanche step: decorrelates the bits of x. */
inline uint64_t avalanche (uint64_t x)
{
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

/** Hash a seed and up to four event coordinates into one decorrelated word. */
inline uint64_t mix (uint64_t seed, uint64_t a = 0, uint64_t b = 0,
                     uint64_t c = 0, uint64_t d = 0)
{
    uint64_t h = avalanche (seed);
    h = avalanche (h ^ a);
    h = avalanche (h ^ b);
    h = avalanche (h ^ c);
    h = avalanche (h ^ d);
    return h;
}

/** Uniform draw in [0, 1) from the hash of (seed, a, b, c, d). */
inline float u01 (uint64_t seed, uint64_t a = 0, uint64_t b = 0,
                  uint64_t c = 0, uint64_t d = 0)
{
    // Top 24 bits -> float mantissa: exact, uniform in [0, 1).
    return (float) (mix (seed, a, b, c, d) >> 40) * (1.0f / 16777216.0f);
}

/** Pick an index proportionally to `weights` (non-negative, need not be
    normalised) with a uniform draw u in [0, 1). Returns -1 if every weight
    is zero (or n <= 0). */
inline int weightedChoice (const float* weights, int n, float u)
{
    float total = 0.0f;
    for (int i = 0; i < n; ++i)
        total += weights[i] > 0.0f ? weights[i] : 0.0f;
    if (! (total > 0.0f))
        return -1;

    float target = u * total;
    for (int i = 0; i < n; ++i)
    {
        const float w = weights[i] > 0.0f ? weights[i] : 0.0f;
        if (target < w)
            return i;
        target -= w;
    }
    return n - 1;   // u ~ 1.0 edge case
}

} // namespace fxme::detrand
