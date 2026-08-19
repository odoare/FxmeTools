/*
  ------------------------------------------------------------------------------
    AllpassChain.h

    Cascade of first-order allpass sections sharing one break frequency — the
    phase-shifting core of a phaser. Each section passes every frequency at
    unit gain but delays its phase by 0 to 180 degrees, crossing 90 degrees at
    the break frequency:

        H(z) = (a + z^-1) / (1 + a·z^-1),   a = (1 - t) / (1 + t),
                                            t = tan(pi·fc/fs)

    Summed with the dry signal, the cascade produces one notch per pair of
    sections; sweeping fc with an LFO moves the notches, which is the whole
    effect. An even number of stages is the usual choice (4, 6, 8, ...) since
    a notch needs 180 degrees of shift.

    setFrequency() is cheap enough to call per sample, which is what a smooth
    sweep wants. Use one instance per channel.

    Threading: prepare() only sets the sample rate and touches no memory, so
    the whole class is realtime-safe. Header-only, <array>/<cmath> only.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace fxme
{

class AllpassChain
{
public:
    static constexpr int maxStages = 16;

    void prepare (double sampleRate)
    {
        sr = sampleRate > 0.0 ? sampleRate : 44100.0;
        nyquistGuard = (float) (0.45 * sr);
        reset();
    }

    void reset()
    {
        xz.fill (0.0f);
        yz.fill (0.0f);
    }

    /** Number of allpass sections actually run. Clamped to [1, maxStages];
        the states of the unused ones are cleared so re-enabling a stage never
        replays a stale sample. */
    void setNumStages (int n) noexcept
    {
        const int wanted = std::clamp (n, 1, maxStages);
        for (int i = wanted; i < stages; ++i)
        {
            xz[(size_t) i] = 0.0f;
            yz[(size_t) i] = 0.0f;
        }
        stages = wanted;
    }

    int getNumStages() const noexcept { return stages; }

    /** Break frequency (the 90-degree point of each section), in Hz. */
    void setFrequency (float hz) noexcept
    {
        const float fc = std::clamp (hz, 20.0f, nyquistGuard);
        const float t  = std::tan (3.14159265358979f * fc / (float) sr);
        a = (1.0f - t) / (1.0f + t);
    }

    float process (float x) noexcept
    {
        for (int i = 0; i < stages; ++i)
        {
            const auto  s = (size_t) i;
            const float y = a * x + xz[s] - a * yz[s];
            xz[s] = x;
            yz[s] = y;
            x     = y;
        }
        return x;
    }

private:
    double sr           = 44100.0;
    float  nyquistGuard = 19845.0f;
    int    stages       = 4;
    float  a            = 0.0f;

    std::array<float, (size_t) maxStages> xz {}, yz {};
};

} // namespace fxme
