/*
  ------------------------------------------------------------------------------
    synth/CurveAdsr.h

    ADSR envelope with a curvature per segment, for amplitude envelopes (run
    per sample) and modulation envelopes (advanced a control block at a time).

    Each segment goes from its start level to its end level along
        shape (x, c) = (exp (k c x) - 1) / (exp (k c) - 1),   k = 6
    for x in [0, 1]: c = 0 is a straight line, c < 0 moves fast first then
    slows down (the usual "exponential" attack and release), c > 0 starts
    slowly. Attack starts from wherever the envelope is (a retrigger never
    jumps to zero) and release from the current level, whatever the segment.

    Times are in seconds; zero is instantaneous. Header-only, no allocation,
    realtime safe.

    Cost: one sample at a time (getNextSample) the curve's exponential is
    carried from sample to sample by one multiplication (in double, so even a
    long segment does not drift); it is recomputed when a segment starts,
    when its time or curve changes, or for a multi-sample advance.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <cmath>

namespace fxme
{

class CurveAdsr
{
public:
    enum class State { idle, attack, decay, sustain, release };

    struct Parameters
    {
        float attack = 0.005f, decay = 0.2f, sustain = 0.8f, release = 0.3f;   // s, s, level, s
        float attackCurve = 0.0f, decayCurve = 0.0f, releaseCurve = 0.0f;     // -1 .. 1
    };

    /** The segment shape described in the file comment. */
    static float shape (float x, float c) noexcept
    {
        if (x <= 0.0f) return 0.0f;
        if (x >= 1.0f) return 1.0f;
        if (std::abs (c) < 1.0e-3f)
            return x;
        const float k = 6.0f * c;
        return (std::exp (k * x) - 1.0f) / (std::exp (k) - 1.0f);
    }

    void setSampleRate (double sampleRate) noexcept { sr = sampleRate > 0.0 ? sampleRate : 44100.0; updateIncrements(); }

    void setParameters (const Parameters& p) noexcept
    {
        params = p;
        params.sustain = params.sustain < 0.0f ? 0.0f : (params.sustain > 1.0f ? 1.0f : params.sustain);
        updateIncrements();
    }

    const Parameters& getParameters() const noexcept { return params; }

    /** Starts (or restarts) the attack from the current level. */
    void noteOn() noexcept
    {
        startLevel = value;
        x = 0.0;
        expValid = false;
        state = State::attack;
        if (attackInc <= 0.0f)
            enterDecay();
    }

    /** Starts the release from the current level (unless already idle). */
    void noteOff() noexcept
    {
        if (state == State::idle || state == State::release)
            return;
        startLevel = value;
        x = 0.0;
        expValid = false;
        state = State::release;
        if (releaseInc <= 0.0f)
            finish();
    }

    void reset() noexcept { state = State::idle; value = 0.0f; x = 0.0; expValid = false; }

    bool isActive() const noexcept { return state != State::idle; }
    State getState() const noexcept { return state; }
    float getValue() const noexcept { return value; }

    /** One sample. */
    float getNextSample() noexcept { return advance (1); }

    /** `numSamples` samples at once (control rate); returns the new value. A
        segment boundary inside the step is handled, the remainder of the step
        carried into the next segment. */
    float advance (int numSamples) noexcept
    {
        float steps = (float) numSamples;
        for (int guard = 0; guard < 4 && steps > 0.0f; ++guard)
        {
            switch (state)
            {
                case State::idle:
                    value = 0.0f;
                    return value;

                case State::sustain:
                    value = params.sustain;
                    return value;

                case State::attack:   steps = step (steps, attackInc, startLevel, 1.0f, params.attackCurve); break;
                case State::decay:    steps = step (steps, decayInc, 1.0f, params.sustain, params.decayCurve); break;
                case State::release:  steps = step (steps, releaseInc, startLevel, 0.0f, params.releaseCurve); break;
            }
        }
        return value;
    }

private:
    /** Moves along the current segment; returns the steps left over when the
        segment ended inside this step. */
    float step (float steps, float inc, float from, float to, float curve) noexcept
    {
        if (inc <= 0.0f)              // the time was set to zero mid-segment
        {
            value = to;
            endSegment();
            return steps;
        }

        const double remaining = (1.0 - x) / (double) inc;
        if ((double) steps < remaining)
        {
            x += (double) steps * (double) inc;
            value = from + (to - from) * segmentShape (steps, inc, curve);
            return 0.0f;
        }
        value = to;
        endSegment();
        return steps - (float) remaining;
    }

    /** shape (x, curve), the exponential carried from the previous sample
        when this is one more sample of the same segment, time and curve. */
    float segmentShape (float steps, float inc, float curve) noexcept
    {
        if (std::abs (curve) < 1.0e-3f)
            return (float) x;
        const double k = 6.0 * (double) curve;
        const bool same = expValid && ! (inc < expInc || inc > expInc) && ! (curve < expCurve || curve > expCurve);
        if (same && ! (steps < 1.0f || steps > 1.0f))
            expX *= expStep;
        else
        {
            expX = std::exp (k * x);
            if (! same)
            {
                expStep = std::exp (k * (double) inc);
                expDenominator = 1.0 / (std::exp (k) - 1.0);
                expInc = inc;
                expCurve = curve;
                expValid = true;
            }
        }
        const double y = (expX - 1.0) * expDenominator;
        return (float) (y < 0.0 ? 0.0 : (y > 1.0 ? 1.0 : y));
    }

    void endSegment() noexcept
    {
        switch (state)
        {
            case State::attack:  enterDecay(); break;
            case State::decay:   state = State::sustain; value = params.sustain; break;
            case State::release: finish(); break;
            case State::idle:
            case State::sustain: break;
        }
    }

    void enterDecay() noexcept
    {
        value = 1.0f;
        x = 0.0;
        expValid = false;
        state = State::decay;
        if (decayInc <= 0.0f)
        {
            state = State::sustain;
            value = params.sustain;
        }
    }

    void finish() noexcept { state = State::idle; value = 0.0f; x = 0.0; expValid = false; }

    void updateIncrements() noexcept
    {
        auto inc = [this] (float seconds) { return seconds <= 0.0f ? 0.0f : (float) (1.0 / (seconds * sr)); };
        attackInc = inc (params.attack);
        decayInc = inc (params.decay);
        releaseInc = inc (params.release);
    }

    double sr = 44100.0;
    Parameters params;
    State state = State::idle;
    float value = 0.0f, startLevel = 0.0f;
    double x = 0.0;                    // position in the segment, 0 to 1
    float attackInc = 0.0f, decayInc = 0.0f, releaseInc = 0.0f;

    // The segment's exponential, carried sample to sample.
    double expX = 1.0, expStep = 1.0, expDenominator = 1.0;
    float expInc = 0.0f, expCurve = 0.0f;
    bool expValid = false;
};

} // namespace fxme
