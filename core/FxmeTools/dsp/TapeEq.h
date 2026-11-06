/*
  ------------------------------------------------------------------------------
    TapeEq.h

    The equalisation around a tape's magnetic stage (fxme::TapeHysteresis),
    per tape speed and EQ standard: what a tape machine does to the signal
    besides saturating it.

      Emphasis   The standards' time constants (timeConstants()):
                   NAB          50 us and 3180 us, at 7.5 and 15 ips
                   IEC (CCIR)   70 us at 7.5 ips, 35 us at 15 ips
                   AES          17.5 us at 30 ips (both standards)
                 EmphasisFilter is the record pre-emphasis (record = true):
                 a first-order high shelf rising from 1 / (2 pi tau_hf), held
                 to +18 dB (the second corner at tau_hf / 8), and with NAB a
                 low shelf rising under 1 / (2 pi tau_lf) (50 Hz), held to
                 +10 dB; or its exact inverse, the playback de-emphasis
                 (record = false). Both come from the same analog prototype
                 through the bilinear transform, so the pair cancels exactly
                 at any sample rate: at low level the machine is flat, and
                 only what the hysteresis did between them remains. That is
                 how highs (and, with NAB, the deep lows) saturate first.

      Head loss  The playback head's high-frequency loss (headLossDb()): the
                 gap loss, sin (x) / x with x = pi g f / v, and the spacing
                 loss, 54.6 d / lambda dB with lambda = v / f, for an
                 effective gap g of 2 um and spacing d of 0.5 um, as left
                 after the machine's alignment. headLossCutoffHz() is where
                 it reaches -3 dB, for a second-order low-pass standing in
                 for it: about 17.5 kHz at 7.5 ips, twice that at 15.

      Head bump  The low-frequency ripple of the playback head (its finite
                 length against long wavelengths): a resonant bump whose
                 frequency scales with the speed, a smaller dip above it, and
                 a roll-off further down (headBumpFor()).

    Header-only, <cmath> only. EmphasisFilter is realtime safe (no
    allocation); the static functions are pure.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <cmath>

namespace fxme::tape
{

enum class Speed { ips7_5 = 0, ips15, ips30 };
enum class EqStandard { nab = 0, iec };

/** Tape speed in metres per second. */
inline double speedMetresPerSecond (Speed s) noexcept
{
    switch (s)
    {
        case Speed::ips7_5: return 7.5 * 0.0254;
        case Speed::ips15:  return 15.0 * 0.0254;
        case Speed::ips30:  return 30.0 * 0.0254;
    }
    return 15.0 * 0.0254;
}

struct TimeConstants
{
    double hf = 50.0e-6;    // seconds
    double lf = 0.0;        // seconds, 0 = none
};

inline TimeConstants timeConstants (Speed s, EqStandard e) noexcept
{
    if (s == Speed::ips30)
        return { 17.5e-6, 0.0 };                        // AES, either standard
    if (e == EqStandard::nab)
        return { 50.0e-6, 3180.0e-6 };
    return { s == Speed::ips7_5 ? 70.0e-6 : 35.0e-6, 0.0 };
}

//==============================================================================
/** Record pre-emphasis or playback de-emphasis, one channel. */
class EmphasisFilter
{
public:
    static constexpr double hfShelfRatio = 8.0;     // +18 dB
    static constexpr double lfShelfRatio = 3.16;    // +10 dB

    void setup (Speed s, EqStandard e, bool record, double sampleRate) noexcept
    {
        const auto tc = timeConstants (s, e);
        const double fs = sampleRate > 0.0 ? sampleRate : 48000.0;

        // High shelf: (1 + s tau) / (1 + s tau / R).
        hf = firstOrder (tc.hf, 1.0, tc.hf / hfShelfRatio, 1.0, fs, record);

        // Low shelf (NAB): (1 + s tau) / (1 / R + s tau) -> R at DC, 1 above.
        hasLf = tc.lf > 0.0;
        if (hasLf)
            lf = firstOrder (tc.lf, 1.0, tc.lf, 1.0 / lfShelfRatio, fs, record);
        reset();
    }

    void reset() noexcept { hf.z = lf.z = 0.0; }

    float process (float x) noexcept
    {
        double y = hf.process (x);
        if (hasLf)
            y = lf.process (y);
        return (float) y;
    }

    /** The analog prototype's magnitude in dB at `hz` (record curve; the
        playback one is its negative). For drawing and tests. */
    static double recordMagnitudeDb (double hz, Speed s, EqStandard e) noexcept
    {
        const auto tc = timeConstants (s, e);
        const double w = 2.0 * 3.141592653589793 * hz;
        auto mag = [w] (double tz, double cz, double tp, double cp)
        {
            return std::sqrt ((cz * cz + (w * tz) * (w * tz)) / (cp * cp + (w * tp) * (w * tp)));
        };
        double m = mag (tc.hf, 1.0, tc.hf / hfShelfRatio, 1.0);
        if (tc.lf > 0.0)
            m *= mag (tc.lf, 1.0, tc.lf, 1.0 / lfShelfRatio);
        return 20.0 * std::log10 (m);
    }

private:
    struct Section
    {
        double b0 = 1.0, b1 = 0.0, a1 = 0.0, z = 0.0;

        double process (double x) noexcept
        {
            const double y = b0 * x + z;
            z = b1 * x - a1 * y;
            return y;
        }
    };

    /** (cz + s tz) / (cp + s tp) through the bilinear transform, or its
        inverse when `forward` is false. */
    static Section firstOrder (double tz, double cz, double tp, double cp, double fs, bool forward) noexcept
    {
        if (! forward)
        {
            double t = tz; tz = tp; tp = t;
            t = cz; cz = cp; cp = t;
        }
        const double K = 2.0 * fs;
        const double n0 = cz + tz * K, n1 = cz - tz * K;
        const double d0 = cp + tp * K, d1 = cp - tp * K;
        Section sec;
        sec.b0 = n0 / d0;
        sec.b1 = n1 / d0;
        sec.a1 = d1 / d0;
        return sec;
    }

    Section hf, lf;
    bool hasLf = false;
};

//==============================================================================
// Playback head loss.

inline constexpr double headGapMetres     = 2.0e-6;
inline constexpr double headSpacingMetres = 0.5e-6;

/** Gap and spacing loss at `hz`, in dB (<= 0). */
inline double headLossDb (double hz, Speed s) noexcept
{
    const double v = speedMetresPerSecond (s);
    const double x = 3.141592653589793 * headGapMetres * hz / v;
    const double gap = x < 1.0e-9 ? 1.0 : std::abs (std::sin (x) / x);
    const double spacingDb = -54.6 * headSpacingMetres * hz / v;
    return 20.0 * std::log10 (gap > 1.0e-9 ? gap : 1.0e-9) + spacingDb;
}

/** Where headLossDb() reaches -3 dB (bisection over 1 kHz .. 200 kHz). */
inline double headLossCutoffHz (Speed s) noexcept
{
    double lo = 1000.0, hi = 200000.0;
    for (int i = 0; i < 60; ++i)
    {
        const double mid = std::sqrt (lo * hi);
        (headLossDb (mid, s) > -3.0 ? lo : hi) = mid;
    }
    return std::sqrt (lo * hi);
}

//==============================================================================
// Head bump.

struct HeadBump
{
    float freqHz, gainDb, q;          // the bump
    float dipHz, dipDb, dipQ;         // the dip above it
    float lowCutHz;                   // second-order roll-off under it
};

inline HeadBump headBumpFor (Speed s) noexcept
{
    switch (s)
    {
        case Speed::ips7_5: return { 45.0f,  2.5f, 1.0f, 110.0f, -1.0f, 1.4f, 15.0f };
        case Speed::ips15:  return { 65.0f,  2.0f, 1.0f, 160.0f, -1.0f, 1.4f, 20.0f };
        case Speed::ips30:  return { 100.0f, 1.5f, 1.0f, 250.0f, -0.8f, 1.4f, 28.0f };
    }
    return { 65.0f, 2.0f, 1.0f, 160.0f, -1.0f, 1.4f, 20.0f };
}

} // namespace fxme::tape
