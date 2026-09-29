/*
  ------------------------------------------------------------------------------
    LevelMatcher.h

    Automatic gain compensation: brings a processed signal back to the RMS
    level of a reference (typically the same signal before the processing),
    so a control that changes loudness as a side effect (a saturator's drive,
    most of all) only changes the character.

    A static compensation cannot do this for a saturator. Dividing by the
    drive is right for small signals, which the curve amplifies by the drive,
    but a signal that clips is capped at the rail whatever the drive, so the
    division then takes off as many dB as the drive adds, and the output gets
    quieter as the drive goes up. Measuring is the only thing that holds for
    every input level.

    Both signals' mean squares are followed by the same one-pole smoother
    (default 200 ms), and the gain is the square root of their ratio, within
    a clamped range. While the reference is silent the last gain is held, so
    a pause does not reset it and the next note starts at the right level.

    The gain moves at the pace of the followers, so a sudden change of what
    the processing does (the drive jumped) is corrected over about the time
    constant. Keep the processing's own static compensation where it has one
    (the division by the drive, for a saturator): the matcher then only has
    the remainder to correct, and small signals are right from the first
    sample.

    Threading: prepare() and the setters from the message thread or before
    processing; process() is realtime safe (no allocation, no locks).

    Usage:

        matcher.prepare (sampleRate);
        ...
        const float y = saturator.processSample (x * drive) / drive;
        out = matcher.process (x, y);

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/util/Math.h>
#include <cmath>

namespace fxme
{

class LevelMatcher
{
public:
    LevelMatcher() = default;

    /** Sets the sample rate and resets. */
    void prepare (double sampleRateIn)
    {
        sampleRate = sampleRateIn > 0.0 ? sampleRateIn : 48000.0;
        setTimeSeconds (timeSeconds);
        reset();
    }

    /** Back to unity gain, with nothing measured. */
    void reset() noexcept
    {
        referenceMs = processedMs = 0.0f;
        gain = 1.0f;
    }

    /** Time constant of the two level followers (default 0.2 s). Longer is
        steadier; shorter follows a changing drive faster, but starts to act
        like a compressor on the signal's own dynamics. */
    void setTimeSeconds (float seconds) noexcept
    {
        timeSeconds = fxme::jmax (0.001f, seconds);
        coef = 1.0f - (float) std::exp (-1.0 / ((double) timeSeconds * sampleRate));
    }

    /** Range the gain is kept within, in dB (default -24 to +60). A saturator
        driven by D dB with a loud input needs a little more than D dB. */
    void setGainRangeDb (float minDb, float maxDb) noexcept
    {
        minGain = fxme::Decibels::decibelsToGain (fxme::jmin (minDb, maxDb), -200.0f);
        maxGain = fxme::Decibels::decibelsToGain (fxme::jmax (minDb, maxDb), -200.0f);
    }

    /** One sample: follows both levels and returns `processed` brought to the
        reference's level. */
    float process (float reference, float processed) noexcept
    {
        referenceMs += coef * (reference * reference - referenceMs);
        processedMs += coef * (processed * processed - processedMs);

        // Below silenceMs the ratio means nothing (and would be noise over
        // noise): keep the gain found while there was signal.
        if (referenceMs > silenceMs && processedMs > silenceMs)
            gain = fxme::jlimit (minGain, maxGain, std::sqrt (referenceMs / processedMs));

        return processed * gain;
    }

    /** The gain currently applied (linear). */
    float getGain() const noexcept { return gain; }

private:
    static constexpr float silenceMs = 1.0e-10f;   // -100 dB RMS

    double sampleRate = 48000.0;
    float timeSeconds = 0.2f;
    float coef = 0.0f;
    float referenceMs = 0.0f, processedMs = 0.0f;
    float gain = 1.0f;
    float minGain = 0.0630957f;   // -24 dB
    float maxGain = 1000.0f;      // +60 dB
};

} // namespace fxme
