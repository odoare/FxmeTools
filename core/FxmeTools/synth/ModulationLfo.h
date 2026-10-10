/*
  ------------------------------------------------------------------------------
    synth/ModulationLfo.h

    The LFO of a synth's modulation section, advanced a control block at a
    time. On top of fxme::Lfo's shapes it has the two random ones, a start
    phase, a delay and a fade-in after the note starts, unipolar or bipolar
    output, and both rate forms (Hz, or beats against a tempo).

      Shapes: sine, triangle, saw up, saw down, square (fxme::Lfo), then
      sample & hold (a new random value each cycle) and smooth random (a
      cosine glide from one random value to the next over each cycle).

    Two ways to run it, chosen by the owner:

      per voice   noteOn() restarts phase, delay and fade (retrigger);
      global      free running; syncToPpq() locks the phase to the host
                  position while the transport rolls.

    Header-only, no allocation, realtime safe.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/dsp/Lfo.h>
#include <FxmeTools/util/Math.h>
#include <FxmeTools/util/Random.h>
#include <cmath>

namespace fxme
{

class ModulationLfo
{
public:
    enum Shape { sine = 0, triangle, sawUp, sawDown, square, sampleAndHold, smoothRandom };

    static constexpr const char* const shapeNames[] = { "Sine", "Triangle", "Saw up", "Saw down",
                                                        "Square", "S&H", "Smooth random" };
    static constexpr int numShapes = (int) (sizeof (shapeNames) / sizeof (shapeNames[0]));

    struct Parameters
    {
        int shape = sine;
        float rateHz = 1.0f;
        bool synced = false;
        float syncBeats = 1.0f;     // beats per cycle when synced
        float startPhase = 0.0f;    // 0 .. 1
        float delaySeconds = 0.0f;
        float fadeSeconds = 0.0f;
        bool bipolar = true;
    };

    ModulationLfo() { random.setSeedRandomly(); next = random.nextBipolar(); current = random.nextBipolar(); }

    void setSampleRate (double sampleRate) noexcept { sr = sampleRate > 0.0 ? sampleRate : 44100.0; }
    void setParameters (const Parameters& p) noexcept { params = p; }
    void setBpm (double newBpm) noexcept { bpm = newBpm > 1.0 ? newBpm : 120.0; }

    /** Restart: phase to the start phase, delay and fade from zero. The phase
        is set by the next advance(), from the parameters current then: a
        caller restarting the LFO before giving it this block's parameters
        (the usual order at a note-on) still gets this note's start phase. */
    void noteOn() noexcept
    {
        restartPending = true;
        elapsed = 0.0;
        current = random.nextBipolar();
        next = random.nextBipolar();
    }

    /** Global LFOs: lock the phase to the host position (in quarter notes). */
    void syncToPpq (double ppq) noexcept
    {
        const double beats = params.synced ? (double) params.syncBeats : bpm / 60.0 / (double) params.rateHz;
        if (beats <= 0.0)
            return;
        const double cycles = ppq / beats + (double) params.startPhase;
        const double newPhase = cycles - std::floor (cycles);
        if (newPhase < phase - 0.5 && ! restartPending)   // wrapped: draw the next random value
            advanceRandom();
        restartPending = false;
        phase = newPhase;
        elapsed = 1.0e9;                // no delay or fade when following the host
    }

    double frequencyHz() const noexcept
    {
        if (params.synced)
            return params.syncBeats > 0.0f ? bpm / 60.0 / (double) params.syncBeats : 0.0;
        return (double) params.rateHz;
    }

    /** Advances by `numSamples` and returns the new output: in [-1, 1] when
        bipolar, [0, 1] when not, scaled by the delay / fade-in. */
    float advance (int numSamples) noexcept
    {
        const double dt = (double) numSamples / sr;
        if (restartPending)
        {
            phase = wrap (params.startPhase);
            restartPending = false;
        }
        elapsed += dt;

        if (elapsed >= (double) params.delaySeconds)
        {
            phase += frequencyHz() * dt;
            if (phase >= 1.0)
            {
                phase -= std::floor (phase);
                advanceRandom();
            }
        }
        return output();
    }

    /** The output for the current phase, without advancing. */
    float output() const noexcept
    {
        float v = 0.0f;
        switch (params.shape)
        {
            case sine:          v = Lfo::eval (Lfo::sine, (float) phase); break;
            case triangle:      v = Lfo::eval (Lfo::triangle, (float) phase); break;
            case sawUp:         v = Lfo::eval (Lfo::sawUp, (float) phase); break;
            case sawDown:       v = Lfo::eval (Lfo::sawDown, (float) phase); break;
            case square:        v = Lfo::eval (Lfo::square, (float) phase); break;
            case sampleAndHold: v = current; break;
            case smoothRandom:
            {
                const float t = 0.5f - 0.5f * std::cos (MathConstants<float>::pi * (float) phase);
                v = current + (next - current) * t;
                break;
            }
            default: break;
        }

        if (! params.bipolar)
            v = 0.5f * (v + 1.0f);

        return v * envelope();
    }

    double getPhase() const noexcept { return phase; }

private:
    float envelope() const noexcept
    {
        const double t = elapsed - (double) params.delaySeconds;
        if (t < 0.0)
            return 0.0f;
        if (params.fadeSeconds <= 0.0f || t >= (double) params.fadeSeconds)
            return 1.0f;
        return (float) (t / (double) params.fadeSeconds);
    }

    void advanceRandom() noexcept
    {
        current = next;
        next = random.nextBipolar();
        if (params.shape == sampleAndHold)
            current = next;
    }

    static double wrap (double p) noexcept { return p - std::floor (p); }

    double sr = 44100.0, bpm = 120.0;
    Parameters params;
    double phase = 0.0, elapsed = 0.0;
    bool restartPending = false;
    float current = 0.0f, next = 0.0f;
    Random random;
};

} // namespace fxme
