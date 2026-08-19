/*
  ------------------------------------------------------------------------------
    util/SmoothedValue.h

    Framework-free linear parameter ramp, replacing JUCE's SmoothedValue in the
    core DSP.

    Only the linear smoothing type is provided, because that is the one the DSP
    uses; a multiplicative ramp can be added if something ever needs it. The
    semantics are otherwise copied from JUCE's, step for step, because a ramp
    that differs by one sample or that lands on its target a step early is
    audible as a click or as zipper noise and would never show up as a build
    failure:

      - reset (sampleRate, seconds) floors the step count, and snaps the value
        to its current target rather than starting a ramp,
      - setTargetValue on a zero-length ramp jumps immediately,
      - getNextValue counts down first, then either steps or snaps exactly onto
        the target on the final call, so the target is reached exactly and not
        approached by accumulated addition.

    One deliberate deviation: JUCE compares the incoming target against the
    current one with an approximate equality, this uses exact equality. The two
    differ only for targets less than a float epsilon apart, where the ramp
    destination is identical either way; the cost is that such a call restarts
    the ramp instead of being ignored.

    No allocation, no locking, no exceptions: safe on the audio thread.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <cmath>

namespace fxme
{

template <typename FloatType>
class SmoothedValue
{
public:
    SmoothedValue() = default;

    /** Starts at `initialValue`, not smoothing. */
    explicit SmoothedValue (FloatType initialValue) noexcept
    {
        setCurrentAndTargetValue (initialValue);
    }

    //==============================================================================
    /** Sets the ramp length. The value itself is left where it is — it snaps to
        its current target, matching JUCE, so a reset never leaves a ramp part
        way through. */
    void reset (double sampleRate, double rampLengthInSeconds) noexcept
    {
        if (sampleRate <= 0.0 || rampLengthInSeconds < 0.0)
            return;

        reset (static_cast<int> (std::floor (rampLengthInSeconds * sampleRate)));
    }

    /** Ramp length directly in samples. */
    void reset (int numSteps) noexcept
    {
        stepsToTarget = numSteps;
        setCurrentAndTargetValue (target);
    }

    //==============================================================================
    void setCurrentAndTargetValue (FloatType newValue) noexcept
    {
        target = currentValue = newValue;
        countdown = 0;
    }

    /** Aims at a new value over the configured ramp length. With a ramp length
        of zero this jumps, which is what makes an un-reset instance behave as a
        plain value rather than freezing at its initial one. */
    void setTargetValue (FloatType newValue) noexcept
    {
        if (newValue == target)
            return;

        if (stepsToTarget <= 0)
        {
            setCurrentAndTargetValue (newValue);
            return;
        }

        target    = newValue;
        countdown = stepsToTarget;
        step      = (target - currentValue) / static_cast<FloatType> (countdown);
    }

    //==============================================================================
    /** Advances one sample and returns the new value. */
    FloatType getNextValue() noexcept
    {
        if (! isSmoothing())
            return target;

        --countdown;

        if (isSmoothing())
            currentValue += step;
        else
            currentValue = target;      // exact landing, no accumulated drift

        return currentValue;
    }

    /** Advances `numSamples` steps and returns the value reached. */
    FloatType skip (int numSamples) noexcept
    {
        if (numSamples >= countdown)
        {
            setCurrentAndTargetValue (target);
            return target;
        }

        currentValue += step * static_cast<FloatType> (numSamples);
        countdown    -= numSamples;
        return currentValue;
    }

    //==============================================================================
    FloatType getCurrentValue() const noexcept { return currentValue; }
    FloatType getTargetValue()  const noexcept { return target; }
    bool      isSmoothing()     const noexcept { return countdown > 0; }

private:
    FloatType currentValue {};
    FloatType target {};
    FloatType step {};
    int countdown     = 0;
    int stepsToTarget = 0;
};

} // namespace fxme
