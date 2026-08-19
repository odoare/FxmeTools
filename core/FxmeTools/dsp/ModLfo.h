/*
  ------------------------------------------------------------------------------
    ModLfo.h

    A running LFO for modulation effects: it owns the phase that fxme::Lfo
    (pure shape maths) deliberately does not, and it knows the two ways a
    modulation rate can be expressed —

      free      the rate is a frequency in Hz
      synced    the rate is a musical division, so the cycle length follows
                the host tempo, and setPhaseFromPpq() locks the phase to the
                host timeline so the sweep starts where the bar does

    One instance drives a whole stereo effect: advance() moves the master
    phase by one sample and valueAt() reads the waveform at an offset from
    it, which is how the right channel (and a chorus's extra voices) get
    their own phase without their own oscillators.

        lfo.setBpm (hostBpm);
        if (lfo.isSynced() && transportIsPlaying)
            lfo.setPhaseFromPpq (ppq);          // once per block

        for (int i = 0; i < numSamples; ++i)
        {
            const float mL = lfo.valueAt (0.0f);
            const float mR = lfo.valueAt (spread);
            lfo.advance();
            ...
        }

    No state to allocate, realtime safe. Header-only.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <algorithm>
#include <cmath>

#include "Lfo.h"

namespace fxme
{

class ModLfo
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate > 0.0 ? sampleRate : 44100.0;
        reset();
    }

    void reset (float startPhase = 0.0f) noexcept
    {
        ph = wrap (startPhase);
    }

    void setShape (int s) noexcept              { shape = s; }
    int  getShape() const noexcept              { return shape; }

    /** Free-running rate, in Hz. Ignored while synced. */
    void setFrequency (float hz) noexcept       { rateHz = std::clamp (hz, 0.001f, 200.0f); }

    void setSynced (bool shouldSync) noexcept   { synced = shouldSync; }
    bool isSynced() const noexcept              { return synced; }

    /** Cycle length in beats (quarter notes) when synced — e.g. 4 for a whole
        note, 1/3 for a quarter-note triplet. See Lfo::syncDivisionBeats(). */
    void setSyncBeats (float beatsPerCycle) noexcept
    {
        beats = std::clamp (beatsPerCycle, 0.01f, 128.0f);
    }

    void setBpm (double newBpm) noexcept
    {
        bpm = newBpm > 1.0 ? newBpm : 120.0;
    }

    /** The rate actually in use, in Hz — the free rate, or the tempo-derived
        one while synced. Handy for a GUI read-out. */
    float frequencyHz() const noexcept
    {
        if (! synced)
            return rateHz;

        return (float) (bpm / 60.0) / beats;
    }

    /** Locks the phase to the host timeline. Call once per block (before the
        sample loop) while the transport is rolling and the rate is synced, so
        the sweep is in the same place on every pass over the same bar. */
    void setPhaseFromPpq (double ppqPosition) noexcept
    {
        const double cycles = ppqPosition / (double) beats;
        ph = wrap ((float) (cycles - std::floor (cycles)));
    }

    /** Advances the master phase by one sample. */
    void advance() noexcept
    {
        ph = wrap (ph + frequencyHz() / (float) sr);
    }

    /** Advances the master phase by a whole block at once.

        For a consumer that reads the LFO per sample this is just the loop
        above written once; the reason it exists is the consumer that reads it
        only *once* per block — a per-block modulation target that is then
        ramped across the block, say — for which stepping the phase one sample
        at a time is a loop whose result is thrown away every iteration but the
        last. Equivalent to calling advance() numSamples times, up to the
        float rounding that the repeated additions would accumulate. */
    void advance (int numSamples) noexcept
    {
        if (numSamples > 0)
            ph = wrap (ph + frequencyHz() * (float) numSamples / (float) sr);
    }

    /** Bipolar value in [-1, 1] at `phaseOffset` (in cycles) ahead of the
        master phase. Offset 0 is the master phase itself. */
    float valueAt (float phaseOffset) const noexcept
    {
        return Lfo::eval (shape, wrap (ph + phaseOffset));
    }

    float value() const noexcept  { return valueAt (0.0f); }
    float phase() const noexcept  { return ph; }

private:
    static float wrap (float p) noexcept
    {
        p -= std::floor (p);
        return p >= 1.0f ? 0.0f : (p < 0.0f ? 0.0f : p);
    }

    double sr     = 44100.0;
    double bpm    = 120.0;
    float  ph     = 0.0f;
    float  rateHz = 1.0f;
    float  beats  = 4.0f;
    bool   synced = false;
    int    shape  = Lfo::sine;
};

} // namespace fxme
