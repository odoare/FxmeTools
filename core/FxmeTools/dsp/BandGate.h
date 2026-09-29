/*
  ------------------------------------------------------------------------------
    BandGate.h

    A gate and a ceiling on a whole signal's level, per sample: the
    time-domain counterpart of fxme::SpectralBandSplitter's per-bin gate,
    meant for one band of a filter bank (fxme::FilterBankSplitter).

    The signal passes while its level is above the gate and below the
    ceiling, with the same knee (a smoothstep over the level in dB, kneeDb
    wide, centred on each line) and the same attack and release as the
    spectral gate, so a band's settings mean the same in both. Only the grain
    differs: here the whole band opens or closes, where the spectral gate
    decides bin by bin.

    Level convention. Levels are expressed so that a steady sine reads the
    same as its peak does on a fxme::SpectrumAnalyzer trace (and so as the
    spectral gate sees it): a sine of amplitude A reads A/2, the level of its
    centre bin through a Hann window. In terms of the signal's mean square,
    level^2 = meanSquare / 2. A single tone therefore passes a line drawn at
    the same place in either engine. A noisy band reads higher than its
    trace, since its level sums every bin in it: that is why a consumer
    should show the band's level (getLevelDb()) next to its lines.

    The detector is a one-pole average of the squared signal. Its time must
    cover a period or so of the band's lowest frequency, or a low band's
    level ripples at twice that frequency and the gate chatters:
    setDetectorSeconds(), or detectorSecondsFor (lowHz) for a sensible value.

    Threading: prepare() from the message thread; the rest is realtime safe
    (no allocation) and meant for the audio thread.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/util/Math.h>
#include <algorithm>
#include <cmath>

namespace fxme
{

class BandGate
{
public:
    /** A gate at or below this is fully open; a ceiling at or above
        offCeilingDb is off. The values of fxme::SpectralBandSplitter. */
    static constexpr float openGateDb   = -150.0f;
    static constexpr float offCeilingDb = 150.0f;
    static constexpr float maxKneeDb    = 48.0f;

    void prepare (double sampleRateIn) noexcept
    {
        sampleRate = sampleRateIn > 0.0 ? sampleRateIn : 48000.0;
        setTimes (attackSeconds, releaseSeconds);
        setDetectorSeconds (detectorSeconds);
        reset();
    }

    /** Forgets the level; the gain goes back to 1 if the gate is open, 0
        otherwise (a gated band starts closed and opens with its attack, as
        a spectral bin does). */
    void reset() noexcept
    {
        meanSquare = peakMeanSquare = 0.0f;
        gain = isGating() ? 0.0f : 1.0f;
    }

    /** The two lines in dB (see the level convention in the file comment). */
    void setThresholds (float gateDb, float ceilingDb) noexcept
    {
        if (gateDb == gateDbSet && ceilingDb == ceilingDbSet)
            return;
        gateDbSet    = gateDb;
        ceilingDbSet = ceilingDb;
        updateThresholds();
    }

    /** Width of the transition around both lines, in dB (0: hard switch). */
    void setKnee (float kneeDb) noexcept
    {
        const float k = fxme::jlimit (0.0f, maxKneeDb, kneeDb);
        if (k == knee)
            return;
        knee = k;
        updateThresholds();
    }

    /** How fast the gain rises towards open and falls towards closed. */
    void setTimes (float attackSecondsIn, float releaseSecondsIn) noexcept
    {
        attackSeconds  = fxme::jmax (0.0f, attackSecondsIn);
        releaseSeconds = fxme::jmax (0.0f, releaseSecondsIn);
        attackCoef  = coefFor (attackSeconds);
        releaseCoef = coefFor (releaseSeconds);
    }

    /** Time constant of the level detector (default 5 ms). */
    void setDetectorSeconds (float seconds) noexcept
    {
        detectorSeconds = fxme::jmax (0.0001f, seconds);
        detectorCoef = 1.0f - coefFor (detectorSeconds);
    }

    /** A detector time for a band whose lowest frequency is `lowHz`: about
        one period of it, between 3 and 50 ms. Short enough to follow notes,
        long enough that the level of a low band does not ripple. */
    static float detectorSecondsFor (float lowHz) noexcept
    {
        return fxme::jlimit (0.003f, 0.05f, 1.0f / fxme::jmax (1.0f, lowHz));
    }

    /** Gates `x` in place, following its level. */
    void process (float* x, int numSamples) noexcept
    {
        const bool gating = isGating();

        float peak = peakMeanSquare;

        for (int i = 0; i < numSamples; ++i)
        {
            meanSquare += detectorCoef * (x[i] * x[i] - meanSquare);
            peak = fxme::jmax (peak, meanSquare);

            if (! gating)
                continue;

            const float levelSq = 0.5f * meanSquare;
            const float target = (gateOpen   ? 1.0f : rise (levelSq, gate))
                               * (ceilingOff ? 1.0f : 1.0f - rise (levelSq, ceiling));
            const float coef = target > gain ? attackCoef : releaseCoef;
            gain = target + coef * (gain - target);
            x[i] *= gain;
        }

        peakMeanSquare = peak;

        if (! gating)
            gain = 1.0f;

        // Keeps the detector out of denormals in silence.
        if (meanSquare < 1.0e-20f)
            meanSquare = 0.0f;
    }

    /** The gain applied last, 0 to 1 (1 when neither line is in use). */
    float getGain() const noexcept { return gain; }

    /** The band's level in dB, in the convention above. */
    float getLevelDb() const noexcept
    {
        return 10.0f * std::log10 (fxme::jmax (1.0e-20f, 0.5f * meanSquare));
    }

    /** The highest level the detector reached since the last call, in dB
        (same convention), then starts over. Sample-accurate, where reading
        getLevelDb() now and then would miss a short peak between reads. */
    float takePeakLevelDb() noexcept
    {
        const float db = 10.0f * std::log10 (fxme::jmax (1.0e-20f, 0.5f * peakMeanSquare));
        peakMeanSquare = 0.0f;
        return db;
    }

    /** True when the gate or the ceiling is in use. */
    bool isGating() const noexcept { return ! gateOpen || ! ceilingOff; }

private:
    /** One line as squared-level edges, as in the spectral splitter. */
    struct Threshold
    {
        float loSq = 0.0f, hiSq = 0.0f;
        float invLogSpan = 0.0f;
    };

    static float rise (float levelSq, const Threshold& t) noexcept
    {
        if (levelSq >= t.hiSq)
            return 1.0f;
        if (levelSq <= t.loSq)
            return 0.0f;

        const float x = std::log (levelSq / t.loSq) * t.invLogSpan;
        return x * x * (3.0f - 2.0f * x);
    }

    Threshold edgesFor (float db) const noexcept
    {
        const float level = fxme::Decibels::decibelsToGain (db, -200.0f);
        const float thrSq = level * level;

        Threshold t;
        if (knee <= 0.0f)
        {
            t.loSq = t.hiSq = thrSq;
            return t;
        }

        const float halfRatio = std::pow (10.0f, knee * 0.05f);
        t.loSq = thrSq / halfRatio;
        t.hiSq = thrSq * halfRatio;
        t.invLogSpan = 1.0f / std::log (t.hiSq / t.loSq);
        return t;
    }

    void updateThresholds() noexcept
    {
        const bool wasGating = isGating();

        gateOpen   = gateDbSet <= openGateDb;
        ceilingOff = ceilingDbSet >= offCeilingDb;
        if (! gateOpen)
            gate = edgesFor (gateDbSet);
        if (! ceilingOff)
            ceiling = edgesFor (ceilingDbSet);

        // Starting to gate: from wide open, as the band was a moment ago.
        if (isGating() && ! wasGating)
            gain = 1.0f;
    }

    float coefFor (float seconds) const noexcept
    {
        return seconds <= 0.0f ? 0.0f
                               : (float) std::exp (-1.0 / ((double) seconds * sampleRate));
    }

    double sampleRate = 48000.0;

    float gateDbSet = -1000.0f, ceilingDbSet = 1000.0f, knee = 0.0f;
    Threshold gate, ceiling;
    bool gateOpen = true, ceilingOff = true;

    float attackSeconds = 0.005f, releaseSeconds = 0.080f, detectorSeconds = 0.005f;
    float attackCoef = 0.0f, releaseCoef = 0.0f, detectorCoef = 1.0f;

    float meanSquare = 0.0f, peakMeanSquare = 0.0f;
    float gain = 1.0f;
};

} // namespace fxme
