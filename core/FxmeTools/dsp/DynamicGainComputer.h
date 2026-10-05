/*
  ------------------------------------------------------------------------------
    DynamicGainComputer.h

    The three stages of a feed-forward (or feedback) dynamics processor that
    produces a gain in dB rather than applying one, so that the gain can drive
    anything: a VCA, a dynamic EQ band (fxme::RegaliaMitraEq), a filter.

      LevelDetector     the level of a detector signal, in dBFS: peak (the
                        rectified sample) or RMS (a one-pole average of the
                        power with a time constant, the "RMS window").
      GainComputer      the static curve: from a level to a gain change in
                        dB, in one of four modes, with a threshold, a ratio,
                        a quadratic soft knee and a range.
      GainBallistics    attack and release, in the log (dB) domain, on the
                        computer's output (the gain is smoothed, not the
                        level). Attack is the gain moving away from 0 dB (more
                        action), release the gain coming back to 0 dB.

    The four modes, with x = level - threshold (dB):

      compress         cut above    -(1 - 1/R) x       for x > 0
      expand           cut below     (R - 1) x         for x < 0
      upwardCompress   boost below  -(1 - 1/R) x       for x < 0
      upwardExpand     boost above   (R - 1) x         for x > 0

    The soft knee is the usual quadratic join, kneeDb wide and centred on the
    threshold. Range is a hard clamp on the size of the gain change, in the
    mode's direction: a cut never goes below -range, a boost above +range.
    That clamp is also what keeps an upward mode bounded when its detector is
    fed back from the output it boosts.

    An infinite ratio is allowed (a limiter in compress, a gate down to the
    range in expand).

    Header-only, no allocation, realtime safe. Time constants are given in
    seconds, so behaviour does not depend on the sample rate.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/util/Math.h>
#include <cmath>
#include <limits>

namespace fxme
{

enum class DynamicsMode
{
    compress = 0,       // cut above the threshold
    expand,             // cut below it
    upwardCompress,     // boost below it
    upwardExpand        // boost above it
};

/** True for the modes that boost (positive gain change). */
inline constexpr bool isUpwardMode (DynamicsMode m) noexcept
{
    return m == DynamicsMode::upwardCompress || m == DynamicsMode::upwardExpand;
}

//==============================================================================
class LevelDetector
{
public:
    enum class Mode { peak, rms };

    /** The level a silent detector reports (dBFS). */
    static constexpr float floorDb = -150.0f;

    void prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
        setRmsWindow (rmsSeconds);
        reset();
    }

    void reset() noexcept { power = 0.0; }

    void setMode (Mode m) noexcept { mode = m; }
    Mode getMode() const noexcept  { return mode; }

    /** Time constant of the RMS average. */
    void setRmsWindow (float seconds) noexcept
    {
        rmsSeconds = fxme::jmax (0.0001f, seconds);
        rmsCoef = 1.0 - std::exp (-1.0 / ((double) rmsSeconds * sampleRate));
    }

    /** One detector sample in, its level in dBFS out. */
    float process (float x) noexcept
    {
        return mode == Mode::peak ? peakDb (std::abs (x))
                                  : averagedDb ((double) x * (double) x);
    }

    /** Two channels linked: the larger of the two peaks, or the mean power
        of the two. */
    float processLinked (float l, float r) noexcept
    {
        return mode == Mode::peak
                 ? peakDb (fxme::jmax (std::abs (l), std::abs (r)))
                 : averagedDb (0.5 * ((double) l * (double) l + (double) r * (double) r));
    }

private:
    static float peakDb (float a) noexcept
    {
        return a > 1.0e-7f ? 20.0f * std::log10 (a) : floorDb;
    }

    float averagedDb (double p) noexcept
    {
        power += rmsCoef * (p - power);
        if (power < 1.0e-20)
            power = 0.0;    // keeps the average out of denormals in silence
        return power > 1.0e-15 ? (float) (10.0 * std::log10 (power)) : floorDb;
    }

    double sampleRate = 48000.0;
    Mode mode = Mode::peak;
    float rmsSeconds = 0.01f;
    double rmsCoef = 0.0;
    double power = 0.0;
};

//==============================================================================
class GainComputer
{
public:
    static constexpr float infiniteRatio = std::numeric_limits<float>::infinity();

    void set (DynamicsMode newMode, float thresholdDbIn, float ratio, float kneeDbIn,
              float rangeDbIn) noexcept
    {
        mode = newMode;
        thresholdDb = thresholdDbIn;
        kneeDb = fxme::jmax (0.0f, kneeDbIn);
        rangeDb = fxme::jmax (0.0f, rangeDbIn);
        ratio = fxme::jmax (1.0f, ratio);

        // The slope of the gain change past the knee, in dB per dB.
        const bool inf = ! std::isfinite (ratio);
        switch (mode)
        {
            case DynamicsMode::compress:
            case DynamicsMode::upwardCompress:
                slope = inf ? 1.0f : 1.0f - 1.0f / ratio;
                break;
            case DynamicsMode::expand:
            case DynamicsMode::upwardExpand:
                slope = inf ? maxExpanderSlope : fxme::jmin (maxExpanderSlope, ratio - 1.0f);
                break;
        }
    }

    /** The gain change in dB asked for at `levelDb`: <= 0 for the cutting
        modes, >= 0 for the boosting ones, never past the range. */
    float gainDb (float levelDb) const noexcept
    {
        const float x = levelDb - thresholdDb;
        switch (mode)
        {
            case DynamicsMode::compress:       return fxme::jmax (-rangeDb, -slope * hinge (x));
            case DynamicsMode::expand:         return fxme::jmax (-rangeDb, -slope * hinge (-x));
            case DynamicsMode::upwardCompress: return fxme::jmin ( rangeDb,  slope * hinge (-x));
            case DynamicsMode::upwardExpand:   return fxme::jmin ( rangeDb,  slope * hinge (x));
        }
        return 0.0f;
    }

    DynamicsMode getMode() const noexcept { return mode; }
    float getRangeDb() const noexcept     { return rangeDb; }

private:
    /** max (0, u) with a quadratic join kneeDb wide around 0. */
    float hinge (float u) const noexcept
    {
        const float half = 0.5f * kneeDb;
        if (u <= -half)
            return 0.0f;
        if (u >= half || kneeDb <= 0.0f)
            return u;
        const float t = u + half;
        return t * t / (2.0f * kneeDb);
    }

    /** An expander's slope is capped: an infinite one is a gate, and the
        range does the rest. */
    static constexpr float maxExpanderSlope = 100.0f;

    DynamicsMode mode = DynamicsMode::compress;
    float thresholdDb = 0.0f, kneeDb = 0.0f, rangeDb = 24.0f;
    float slope = 0.5f;
};

//==============================================================================
class GainBallistics
{
public:
    void prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
        setTimes (attackSeconds, releaseSeconds);
        reset();
    }

    void reset (float valueDb = 0.0f) noexcept { current = valueDb; }

    void setTimes (float attack, float release) noexcept
    {
        attackSeconds = fxme::jmax (0.0f, attack);
        releaseSeconds = fxme::jmax (0.0f, release);
        attackCoef = coefFor (attackSeconds);
        releaseCoef = coefFor (releaseSeconds);
    }

    /** Follows `targetDb`; returns the smoothed gain change in dB. */
    float process (float targetDb) noexcept
    {
        const float coef = std::abs (targetDb) > std::abs (current) ? attackCoef : releaseCoef;
        current = targetDb + coef * (current - targetDb);
        if (std::abs (current) < 1.0e-9f)
            current = 0.0f;
        return current;
    }

    float getCurrentDb() const noexcept { return current; }

private:
    float coefFor (float seconds) const noexcept
    {
        return seconds <= 0.0f ? 0.0f : (float) std::exp (-1.0 / ((double) seconds * sampleRate));
    }

    double sampleRate = 48000.0;
    float attackSeconds = 0.005f, releaseSeconds = 0.1f;
    float attackCoef = 0.0f, releaseCoef = 0.0f;
    float current = 0.0f;
};

} // namespace fxme
