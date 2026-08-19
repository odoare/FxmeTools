/*
  ------------------------------------------------------------------------------
    util/Random.h

    Framework-free pseudo-random generator replacing JUCE's Random class in the DSP
    kernels (noise generation, crackle triggering).

    Uses the classic 48-bit linear congruential generator (multiplier
    0x5deece66d, increment 11) popularised by java.util.Random — the same
    well-known algorithm JUCE uses — so the statistical character of generated
    noise is unchanged by the migration. Not cryptographically secure; that is
    not what it is for.

    No allocation, no locking, no exceptions: safe to call from processBlock.
    Not thread-safe: give each thread (and each voice) its own instance.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <cstdint>
#include <limits>

namespace fxme
{

class Random
{
public:
    /** Seeds from a fixed default. Pass a seed for reproducible sequences. */
    explicit Random (std::int64_t seedValue = 0x330e5deece66dLL) noexcept
        : seed (seedValue) {}

    void setSeed (std::int64_t newSeed) noexcept { seed = newSeed; }
    std::int64_t getSeed() const noexcept        { return seed; }

    /** Uniformly distributed 32-bit integer over the full int range. */
    int nextInt() noexcept
    {
        seed = static_cast<std::int64_t> ((static_cast<std::uint64_t> (seed) * 0x5deece66dULL + 11)
                                          & 0xffffffffffffULL);
        return static_cast<int> (seed >> 16);
    }

    /** Uniformly distributed integer in [0, maxValue). Returns 0 if maxValue <= 0. */
    int nextInt (int maxValue) noexcept
    {
        if (maxValue <= 0)
            return 0;

        return static_cast<int> ((static_cast<std::uint64_t> (static_cast<std::uint32_t> (nextInt()))
                                  * static_cast<std::uint64_t> (maxValue)) >> 32);
    }

    /** Uniformly distributed integer in [start, end). */
    int nextInt (int start, int end) noexcept
    {
        return end <= start ? start : start + nextInt (end - start);
    }

    std::int64_t nextInt64() noexcept
    {
        return static_cast<std::int64_t> ((static_cast<std::uint64_t> (static_cast<std::uint32_t> (nextInt())) << 32)
                                          | static_cast<std::uint64_t> (static_cast<std::uint32_t> (nextInt())));
    }

    bool nextBool() noexcept { return (nextInt() & 0x40000000) != 0; }

    /** Uniformly distributed float in [0, 1). */
    float nextFloat() noexcept
    {
        const auto result = static_cast<float> (static_cast<std::uint32_t> (nextInt()))
                          / (static_cast<float> (0xffffffffu) + 1.0f);

        return result >= 1.0f ? 1.0f - std::numeric_limits<float>::epsilon() : result;
    }

    /** Uniformly distributed double in [0, 1). */
    double nextDouble() noexcept
    {
        return static_cast<double> (static_cast<std::uint32_t> (nextInt()))
             / (static_cast<double> (0xffffffffu) + 1.0);
    }

    /** Bipolar float in [-1, 1) — the common case for noise. */
    float nextBipolar() noexcept { return nextFloat() * 2.0f - 1.0f; }

private:
    std::int64_t seed;
};

} // namespace fxme
