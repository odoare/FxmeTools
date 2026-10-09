/*
  ------------------------------------------------------------------------------
    synth/WavetableSet.h

    One band-limited wavetable set: a single-cycle waveform stored as a mipmap
    of tables, one per octave, each holding only the harmonics that stay below
    Nyquist at the highest note it serves. Up to two channels (a stereo source
    gives a stereo set).

        level 0   1023 harmonics   (notes below ~ sampleRate / 2048)
        level 1    512 harmonics
        ...
        level 10     1 harmonic    (a sine)

    Every table is tableSize samples long and is stored with `guard` wrapped
    samples on each side, so an interpolator reading taps i - 3 .. i + 4 needs
    no wrap test: table (ch, level)[-guard .. tableSize + guard - 1] is valid.

    Choosing the level for a note (see levelFor): the "ideal" level is
    log2 (2048 f / fs). Reading the table one level below it lets the top
    harmonic reach sqrt(2) times Nyquist at worst, folding back no lower than
    0.29 fs (about 14 kHz at 48 kHz); reading one level above it never aliases
    but can lose an octave of top harmonics. levelBias = 0.5 sits between the
    two, the usual compromise for wavetable synths.

    Plain data, allocated once (allocate()) on a non-audio thread; reading is
    realtime safe.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

namespace fxme
{

struct WavetableSet
{
    static constexpr int tableOrder  = 11;
    static constexpr int tableSize   = 1 << tableOrder;     // 2048 samples per cycle
    static constexpr int numLevels   = tableOrder;          // 1023, 512, ..., 1 harmonics
    static constexpr int guard       = 4;
    static constexpr int stride      = tableSize + 2 * guard;
    static constexpr int maxChannels = 2;

    /** How far below the alias-free level the reader picks (see file comment). */
    static constexpr float levelBias = 0.5f;

    /** Allocates storage for maxChannels channels (zeroed). Not realtime safe. */
    void allocate()
    {
        data.assign ((std::size_t) (maxChannels * numLevels * stride), 0.0f);
        numChannels = 1;
    }

    bool isAllocated() const noexcept { return ! data.empty(); }

    /** Pointer to sample 0 of a table; indices -guard .. tableSize + guard - 1
        are valid. */
    const float* table (int channel, int level) const noexcept
    {
        return data.data() + (std::size_t) ((channel * numLevels + level) * stride + guard);
    }

    float* table (int channel, int level) noexcept
    {
        return data.data() + (std::size_t) ((channel * numLevels + level) * stride + guard);
    }

    /** Copies the wrapped guard samples of one table from its body. */
    void fillGuards (int channel, int level) noexcept
    {
        float* t = table (channel, level);
        for (int i = 1; i <= guard; ++i)
        {
            t[-i] = t[tableSize - i];
            t[tableSize - 1 + i] = t[i - 1];
        }
    }

    /** Highest harmonic kept in `level`. Level 0 drops the table's own Nyquist
        bin (a cosine at fs/2 of the table, which has no defined phase). */
    static int maxHarmonic (int level) noexcept
    {
        const int h = (tableSize / 2) >> level;
        return h >= tableSize / 2 ? tableSize / 2 - 1 : h;
    }

    /** The (fractional) mip level to read for a fundamental `freqHz`, already
        biased by levelBias and clamped to [0, numLevels - 1]. Its integer
        part is the table to read. */
    static float levelFor (double freqHz, double sampleRate) noexcept
    {
        const double f = freqHz < 1.0e-3 ? 1.0e-3 : freqHz;
        const double lf = std::log2 ((double) tableSize * f / sampleRate) + (double) levelBias;
        if (lf <= 0.0)
            return 0.0f;
        if (lf >= (double) (numLevels - 1))
            return (float) (numLevels - 1);
        return (float) lf;
    }

    std::vector<float> data;
    int numChannels = 1;
};

} // namespace fxme
