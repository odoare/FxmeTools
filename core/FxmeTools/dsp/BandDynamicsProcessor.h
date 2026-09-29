/*
  ------------------------------------------------------------------------------
    BandDynamicsProcessor.h

    The dynamics of one band, on its whole level, per sample: the
    time-domain counterpart of fxme::SpectralBandSplitter's per-bin gate
    and ceiling, meant for one band of a filter bank
    (fxme::FilterBankSplitter). It measures the band's level, asks
    fxme::DynamicsCurve (BandDynamics.h, the stateless maths both splitters
    share) what gain each line wants, follows that with each line's attack
    and release, and applies it. (Formerly fxme::BandGate: it compresses as
    well as gates now.)

    Around its two lines it applies fxme::BandDynamics: under the gate a
    downward expander (a hard gate by default), over the ceiling a compressor
    or a cut (a cut by default). With a compressor ratio this is one band of a
    classic multiband compressor. The default behaves as a gate and a
    ceiling: the signal passes while its level is above the gate and below
    the ceiling, with the same knee (a smoothstep over the level in dB, kneeDb
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

#include <FxmeTools/dsp/BandDynamics.h>
#include <FxmeTools/util/Math.h>
#include <algorithm>
#include <cmath>

namespace fxme
{

class BandDynamicsProcessor
{
public:
    /** A gate at or below this is fully open; a ceiling at or above
        offCeilingDb is off. The values of fxme::SpectralBandSplitter. */
    static constexpr float openGateDb   = -150.0f;
    static constexpr float offCeilingDb = 150.0f;
    static constexpr float maxKneeDb    = DynamicsCurve::maxKneeDb;

    void prepare (double sampleRateIn) noexcept
    {
        sampleRate = sampleRateIn > 0.0 ? sampleRateIn : 48000.0;
        setDynamics (dynamics);
        setDetectorSeconds (detectorSeconds);
        reset();
    }

    /** Forgets the level; the gate's gain goes back to closed if the gate is
        in use (a gated band starts closed and opens with its attack, as a
        spectral bin does), the ceiling's to 1. */
    void reset() noexcept
    {
        meanSquare = peakMeanSquare = 0.0f;
        gateGain = curve.isGateOn() ? 0.0f : 1.0f;
        ceilingGain = 1.0f;
    }

    /** The two lines in dB (see the level convention in the file comment). */
    void setThresholds (float gateDb, float ceilingDb) noexcept
    {
        if (gateDb == gateDbSet && ceilingDb == ceilingDbSet)
            return;
        gateDbSet    = gateDb;
        ceilingDbSet = ceilingDb;
        updateCurve();
    }

    /** What the two lines do (see fxme::BandDynamics): expander ratio and
        range under the gate, compressor ratio or cut over the ceiling, and
        each line's knee, attack and release, followed per sample. */
    void setDynamics (const BandDynamics& d) noexcept
    {
        dynamics = d;
        gateAttackCoef     = coefFor (d.gateAttackSeconds);
        gateReleaseCoef    = coefFor (d.gateReleaseSeconds);
        ceilingAttackCoef  = coefFor (d.ceilingAttackSeconds);
        ceilingReleaseCoef = coefFor (d.ceilingReleaseSeconds);
        updateCurve();
    }

    const BandDynamics& getDynamics() const noexcept { return dynamics; }

    /** Width of the transition around both lines, in dB (0: hard switch). */
    void setKnee (float kneeDb) noexcept
    {
        const float k = fxme::jlimit (0.0f, maxKneeDb, kneeDb);
        if (k == dynamics.gateKneeDb && k == dynamics.ceilingKneeDb)
            return;
        auto d = dynamics;
        d.gateKneeDb = d.ceilingKneeDb = k;
        setDynamics (d);
    }

    /** How fast both lines' gains rise and fall. */
    void setTimes (float attackSecondsIn, float releaseSecondsIn) noexcept
    {
        auto d = dynamics;
        d.gateAttackSeconds  = d.ceilingAttackSeconds  = fxme::jmax (0.0f, attackSecondsIn);
        d.gateReleaseSeconds = d.ceilingReleaseSeconds = fxme::jmax (0.0f, releaseSecondsIn);
        setDynamics (d);
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
        processLinked (x, x, nullptr, numSamples);
    }

    /** Gates `left` and `right` (null for mono) in place with one gain,
        following the level of `detector` (the two channels' average, say),
        so a stereo band opens and closes as one. `detector` may be `left`:
        each detector sample is read before its channel sample is scaled. */
    void processLinked (const float* detector, float* left, float* right, int numSamples) noexcept
    {
        const bool gateOn = curve.isGateOn(), ceilingOn = curve.isCeilingOn();
        float peak = peakMeanSquare;

        for (int i = 0; i < numSamples; ++i)
        {
            const float d = detector[i];
            meanSquare += detectorCoef * (d * d - meanSquare);
            peak = fxme::jmax (peak, meanSquare);

            if (! gateOn && ! ceilingOn)
                continue;

            const float levelSq = 0.5f * meanSquare;

            if (gateOn)
            {
                const float target = curve.gateGain (levelSq);
                gateGain = target + (target > gateGain ? gateAttackCoef : gateReleaseCoef) * (gateGain - target);
            }
            if (ceilingOn)
            {
                const float target = curve.ceilingGain (levelSq);
                // The ceiling acts by pulling the gain down: that is its
                // attack; letting it back up is its release (the gate's the
                // other way round).
                ceilingGain = target + (target < ceilingGain ? ceilingAttackCoef : ceilingReleaseCoef)
                                           * (ceilingGain - target);
            }

            const float g = gateGain * ceilingGain;
            left[i] *= g;
            if (right != nullptr)
                right[i] *= g;
        }

        peakMeanSquare = peak;

        if (! gateOn)
            gateGain = 1.0f;
        if (! ceilingOn)
            ceilingGain = 1.0f;

        // Keeps the detector out of denormals in silence.
        if (meanSquare < 1.0e-20f)
            meanSquare = 0.0f;
    }

    /** The gain applied last, 0 to 1 (1 when neither line is in use). */
    float getGain() const noexcept { return gateGain * ceilingGain; }

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
    bool isGating() const noexcept { return curve.isGateOn() || curve.isCeilingOn(); }

private:
    void updateCurve() noexcept
    {
        const bool gateWasOn = curve.isGateOn();

        curve.set (gateDbSet <= openGateDb ? 0.0f : fxme::Decibels::decibelsToGain (gateDbSet, -200.0f),
                   ceilingDbSet >= offCeilingDb ? BandDynamics::infinity
                                                : fxme::Decibels::decibelsToGain (ceilingDbSet, -200.0f),
                   dynamics);

        // Starting to gate: from wide open, as the band was a moment ago.
        if (curve.isGateOn() && ! gateWasOn)
            gateGain = 1.0f;
    }

    float coefFor (float seconds) const noexcept
    {
        return seconds <= 0.0f ? 0.0f
                               : (float) std::exp (-1.0 / ((double) seconds * sampleRate));
    }

    double sampleRate = 48000.0;

    float gateDbSet = -1000.0f, ceilingDbSet = 1000.0f;
    BandDynamics dynamics;
    DynamicsCurve curve;

    float gateAttackCoef = 0.0f, gateReleaseCoef = 0.0f;
    float ceilingAttackCoef = 0.0f, ceilingReleaseCoef = 0.0f;
    float detectorSeconds = 0.005f, detectorCoef = 1.0f;

    float meanSquare = 0.0f, peakMeanSquare = 0.0f;
    float gateGain = 1.0f, ceilingGain = 1.0f;
};

} // namespace fxme
