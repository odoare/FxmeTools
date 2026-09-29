/*
  ------------------------------------------------------------------------------
    BandDynamics.h

    The dynamics of one band of a band splitter, around its two level lines:
    what happens below the lower one (the gate) and above the upper one (the
    ceiling). Shared by fxme::SpectralBandSplitter, which applies it to every
    frequency bin of a band on its own (a spectral expander / compressor), and
    fxme::BandDynamicsProcessor / fxme::FilterBankSplitter, which apply it to a band's
    overall level (a classic multiband expander / compressor).

      - Gate (the lower line): a downward expander. Every dB under the line
        takes the output `gateRatio` dB under it, so the gain is
        (level - line) * (ratio - 1) dB, never under `gateRangeDb`. An
        infinite ratio is a hard gate: closed under the line (down to the
        range), open over it.
      - Ceiling (the upper line): a compressor. Every dB over the line comes
        out as 1 / ratio dB, so the gain is (line - level) * (1 - 1/ratio) dB;
        an infinite ratio is a limiter. Or, with `ceilingCut`, a cut: what
        rises over the line is muted (the ceiling as it first was).

    Each line has its own knee (a transition that many dB wide, centred on
    the line) and its own attack and release (how fast the gain it asks for
    is followed, down and back up). The defaults (a hard gate with no range,
    a hard cut, 5 ms and 80 ms) are the splitters' original behaviour.

    Levels are compared as squared magnitudes against squared thresholds, in
    whatever unit the caller uses; a logarithm is only taken for levels
    where the answer is not simply 1 (or, for a hard gate or cut, 0 / 1).

    Header-only, realtime safe.

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

/** One band's dynamics settings (the line levels themselves are part of
    fxme::SpectralBand: gateDb and ceilingDb). */
struct BandDynamics
{
    static constexpr float infinity = std::numeric_limits<float>::infinity();

    float gateRatio   = infinity;     // 1 (none) .. infinity (a gate)
    float gateRangeDb = -infinity;    // how far down the gate goes; -inf: silence
    float gateKneeDb  = 0.0f;
    float gateAttackSeconds  = 0.005f;
    float gateReleaseSeconds = 0.080f;

    bool  ceilingCut   = true;        // mute over the line, rather than compress
    float ceilingRatio = infinity;    // when compressing: 1 (none) .. infinity (a limiter)
    float ceilingKneeDb = 0.0f;
    float ceilingAttackSeconds  = 0.005f;
    float ceilingReleaseSeconds = 0.080f;
};

/** A band's two lines turned into gains: prepared once per settings change
    (set()), asked per level (gateGain(), ceilingGain()). */
class DynamicsCurve
{
public:
    static constexpr float maxKneeDb = 48.0f;

    /** `gateThreshold` and `ceilingThreshold` are the lines as linear
        levels in the caller's unit (the squared level is compared with their
        square). A gate at or below 0, or a ceiling that is infinite, is off. */
    void set (float gateThreshold, float ceilingThreshold, const BandDynamics& d) noexcept
    {
        gateOn    = gateThreshold > 0.0f && d.gateRatio > 1.0f && d.gateRangeDb < 0.0f;
        ceilingOn = std::isfinite (ceilingThreshold) && (d.ceilingCut || d.ceilingRatio > 1.0f);

        gateHard  = std::isinf (d.gateRatio);
        gateSlope = gateHard ? 0.0f : d.gateRatio - 1.0f;
        gateRange = std::isinf (d.gateRangeDb) ? 0.0f
                                               : fxme::Decibels::decibelsToGain (d.gateRangeDb, -1000.0f);
        gateRangeDb = d.gateRangeDb;
        gate = lineFor (gateThreshold, d.gateKneeDb);

        ceilingCut   = d.ceilingCut;
        ceilingSlope = std::isinf (d.ceilingRatio) ? 1.0f : 1.0f - 1.0f / d.ceilingRatio;
        ceiling = lineFor (ceilingThreshold, d.ceilingKneeDb);
    }

    bool isGateOn() const noexcept      { return gateOn; }
    bool isCeilingOn() const noexcept   { return ceilingOn; }

    /** The gain the gate asks for at squared level `levelSq`, 0 to 1. */
    float gateGain (float levelSq) const noexcept
    {
        if (! gateOn || levelSq >= gate.hiSq)
            return 1.0f;

        if (gateHard)
        {
            // Closed (to the range) under the knee, a smoothstep across it.
            const float open = levelSq <= gate.loSq ? 0.0f : smoothstep (levelSq, gate);
            return gateRange + (1.0f - gateRange) * open;
        }

        // Expander: (level - line) * (ratio - 1) dB under the knee, the
        // quadratic join across it, never under the range.
        const float d = dbOver (levelSq, gate);
        const float halfKnee = gate.halfKneeDb;
        const float db = d <= -halfKnee || halfKnee <= 0.0f
                           ? gateSlope * d
                           : -gateSlope * (d - halfKnee) * (d - halfKnee) / (4.0f * halfKnee);
        return fxme::Decibels::decibelsToGain (fxme::jmax (db, gateRangeDb), -1000.0f);
    }

    /** The gain the ceiling asks for at squared level `levelSq`, 0 to 1. */
    float ceilingGain (float levelSq) const noexcept
    {
        // A level exactly on a hard line counts as over it, as for the gate.
        if (! ceilingOn || levelSq < ceiling.loSq)
            return 1.0f;

        if (ceilingCut)
            return levelSq >= ceiling.hiSq ? 0.0f : 1.0f - smoothstep (levelSq, ceiling);

        // Compressor: (line - level) * (1 - 1/ratio) dB over the knee, the
        // quadratic join across it.
        const float d = dbOver (levelSq, ceiling);
        const float halfKnee = ceiling.halfKneeDb;
        const float db = d >= halfKnee || halfKnee <= 0.0f
                           ? -ceilingSlope * d
                           : -ceilingSlope * (d + halfKnee) * (d + halfKnee) / (4.0f * halfKnee);
        return fxme::Decibels::decibelsToGain (db, -1000.0f);
    }

private:
    /** A line as squared-level edges of its knee, and what dB conversion
        needs: the threshold's square and the half-knee. */
    struct Line
    {
        float thrSq = 0.0f, loSq = 0.0f, hiSq = 0.0f;
        float halfKneeDb = 0.0f;
        float invLogSpan = 0.0f;      // 1 / ln (hiSq / loSq), 0 when hard
    };

    static Line lineFor (float threshold, float kneeDb) noexcept
    {
        Line l;
        const float knee = fxme::jlimit (0.0f, maxKneeDb, kneeDb);
        if (! std::isfinite (threshold) || threshold <= 0.0f)
        {
            l.thrSq = l.loSq = l.hiSq = std::isfinite (threshold) ? 0.0f : std::numeric_limits<float>::max();
            return l;
        }

        l.thrSq = threshold * threshold;
        l.halfKneeDb = 0.5f * knee;
        if (knee <= 0.0f)
        {
            l.loSq = l.hiSq = l.thrSq;
            return l;
        }

        // Half the knee each side of the line, in dB of level (10 log10 of
        // the squared level).
        const float halfRatio = std::pow (10.0f, knee * 0.05f);
        l.loSq = l.thrSq / halfRatio;
        l.hiSq = l.thrSq * halfRatio;
        l.invLogSpan = 1.0f / std::log (l.hiSq / l.loSq);
        return l;
    }

    /** 0 at the knee's lower edge, 1 at its upper edge, a smoothstep over the
        level in dB between them. Only called inside the knee. */
    static float smoothstep (float levelSq, const Line& l) noexcept
    {
        if (l.hiSq <= l.loSq)
            return levelSq >= l.thrSq ? 1.0f : 0.0f;

        const float x = std::log (levelSq / l.loSq) * l.invLogSpan;
        return x * x * (3.0f - 2.0f * x);
    }

    /** How far the level is over the line, in dB. */
    static float dbOver (float levelSq, const Line& l) noexcept
    {
        return 10.0f * std::log10 (fxme::jmax (levelSq, 1.0e-30f) / l.thrSq);
    }

    bool gateOn = false, ceilingOn = false;
    bool gateHard = true, ceilingCut = true;
    float gateSlope = 0.0f, gateRange = 0.0f, gateRangeDb = -1000.0f;
    float ceilingSlope = 1.0f;
    Line gate, ceiling;
};

} // namespace fxme
