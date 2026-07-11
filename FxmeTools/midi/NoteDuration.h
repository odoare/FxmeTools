/*
  ------------------------------------------------------------------------------
    NoteDuration.h

    Rhythmic note durations (1/4 .. 1/32, straight / triplet / dotted) and a
    weighted-random duration table: instead of picking one duration, the user
    tunes a probability weight per duration and per modifier, and drawBeats()
    resolves an actual duration from a uniform draw. Pairs with
    dsp/DeterministicRandom.h for reproducible draws.

    Header-only, no dependencies; realtime-safe.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

namespace fxme
{

enum class NoteBase { Quarter = 0, Eighth, Sixteenth, ThirtySecond };
enum class NoteMod  { Straight = 0, Triplet, Dotted };

inline constexpr int kNumNoteBases = 4;
inline constexpr int kNumNoteMods  = 3;

/** Duration in quarter-note beats: quarter = 1, dotted x1.5, triplet x2/3. */
inline double noteDurationBeats (NoteBase base, NoteMod mod)
{
    double beats = 1.0;
    switch (base)
    {
        case NoteBase::Quarter:      beats = 1.0;   break;
        case NoteBase::Eighth:       beats = 0.5;   break;
        case NoteBase::Sixteenth:    beats = 0.25;  break;
        case NoteBase::ThirtySecond: beats = 0.125; break;
    }
    switch (mod)
    {
        case NoteMod::Straight: break;
        case NoteMod::Triplet:  beats *= 2.0 / 3.0; break;
        case NoteMod::Dotted:   beats *= 1.5;       break;
    }
    return beats;
}

/** Probability weights (0..1 each) over the 4x3 duration/modifier grid.
    The probability of a combination is proportional to
    baseWeights[base] * modWeights[mod] over the sum of all combinations —
    e.g. with P(1/4)=1, P(1/8)=0.5, P(1/16)=0.1, P(1/32)=0 (straight only),
    the chance of drawing 1/4 is 1 / (1 + 0.5 + 0.1). */
struct WeightedDurationTable
{
    float baseWeights[kNumNoteBases] = { 1.0f, 0.0f, 0.0f, 0.0f };
    float modWeights[kNumNoteMods]   = { 1.0f, 0.0f, 0.0f };

    /** Resolve a duration from a uniform draw u in [0, 1). Falls back to a
        straight quarter note when every combined weight is zero. */
    double drawBeats (float u) const
    {
        float combined[kNumNoteBases * kNumNoteMods];
        float total = 0.0f;
        for (int b = 0; b < kNumNoteBases; ++b)
            for (int m = 0; m < kNumNoteMods; ++m)
            {
                const float w = clamped (baseWeights[b]) * clamped (modWeights[m]);
                combined[b * kNumNoteMods + m] = w;
                total += w;
            }

        if (! (total > 0.0f))
            return 1.0;   // straight quarter

        float target = u * total;
        for (int i = 0; i < kNumNoteBases * kNumNoteMods; ++i)
        {
            if (target < combined[i])
                return noteDurationBeats ((NoteBase) (i / kNumNoteMods),
                                          (NoteMod)  (i % kNumNoteMods));
            target -= combined[i];
        }
        return noteDurationBeats (NoteBase::ThirtySecond, NoteMod::Dotted);   // u ~ 1.0 edge
    }

private:
    static float clamped (float w) { return w < 0.0f ? 0.0f : (w > 1.0f ? 1.0f : w); }
};

} // namespace fxme
