/*
  ------------------------------------------------------------------------------
    SignalGenerator.h

    Simple test-signal generator: a sinusoid and/or white noise, each with its
    own on/off and amplitude (dB). Produces a mono stimulus sample stream;
    routing it to channels is the caller's job.

    Call beginBlock() once per audio block to snapshot the (atomic) settings,
    then nextSample() per sample. Controls are set from the message thread.
    Header-only, depends only on JUCE.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>

namespace fxme
{

class SignalGenerator
{
public:
    SignalGenerator() = default;

    void prepare (double sampleRate)
    {
        sr = sampleRate > 0.0 ? sampleRate : 44100.0;
        sinePhase = 0.0;
    }

    //==========================================================================
    // Message-thread setters (realtime-safe atomics).
    void setSineOn (bool on)        { sineOn.store (on); }
    void setSineAmpDb (float db)    { sineAmpDb.store (db); }
    void setSineFreq (float hz)     { sineFreq.store (hz); }
    void setNoiseOn (bool on)       { noiseOn.store (on); }
    void setNoiseAmpDb (float db)   { noiseAmpDb.store (db); }

    bool isGenerating() const noexcept { return sineOn.load() || noiseOn.load(); }

    //==========================================================================
    /** Audio thread. Snapshots the current settings for the coming block. */
    void beginBlock() noexcept
    {
        blkSine  = sineOn.load();
        blkNoise = noiseOn.load();
        blkSineGain  = juce::Decibels::decibelsToGain (sineAmpDb.load());
        blkNoiseGain = juce::Decibels::decibelsToGain (noiseAmpDb.load());
        blkPhaseInc  = 2.0 * juce::MathConstants<double>::pi * (double) sineFreq.load() / sr;
    }

    /** Audio thread. One mono stimulus sample (sine + noise per beginBlock()). */
    float nextSample() noexcept
    {
        float v = 0.0f;
        if (blkSine)
        {
            v += blkSineGain * (float) std::sin (sinePhase);
            sinePhase += blkPhaseInc;
            if (sinePhase >= juce::MathConstants<double>::twoPi)
                sinePhase -= juce::MathConstants<double>::twoPi;
        }
        if (blkNoise)
            v += blkNoiseGain * (random.nextFloat() * 2.0f - 1.0f);
        return v;
    }

private:
    double sr = 44100.0;

    std::atomic<bool>  sineOn { false }, noiseOn { false };
    std::atomic<float> sineAmpDb { -12.0f }, sineFreq { 1000.0f }, noiseAmpDb { -12.0f };

    // Per-block snapshot.
    bool   blkSine = false, blkNoise = false;
    float  blkSineGain = 0.0f, blkNoiseGain = 0.0f;
    double blkPhaseInc = 0.0;

    double sinePhase = 0.0;
    juce::Random random;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SignalGenerator)
};

} // namespace fxme
