/*
  ==============================================================================

    Lfo.h

    Stateless LFO kernel shared across FX-Mechanics projects: waveform shape
    evaluation and the tempo-sync rate table. Phase management, depth, polarity
    and parameter wiring stay with the consumer (e.g. a modulation engine); this
    only provides the pure maths so the same shapes/sync rates are reused.

  ==============================================================================
*/

#pragma once

#include <FxmeTools/util/Math.h>
#include <cmath>
#include <cstddef>

namespace fxme
{

class Lfo
{
public:
    // Waveform shapes, in the canonical order used by GUI choice lists.
    enum Shape { sine = 0, triangle, square, sawUp, sawDown };

    // Bipolar value in [-1, 1] for a normalised phase in [0, 1).
    static float eval (int shape, float phase) noexcept
    {
        switch (shape)
        {
            case triangle: return phase < 0.5f ? (4.0f * phase - 1.0f) : (3.0f - 4.0f * phase);
            case square:   return phase < 0.5f ? 1.0f : -1.0f;
            case sawUp:    return 2.0f * phase - 1.0f;
            case sawDown:  return 1.0f - 2.0f * phase;
            case sine:
            default:       return std::sin (fxme::MathConstants<float>::twoPi * phase);
        }
    }

    // Beats per LFO cycle for a tempo-sync rate index, one per syncRateNames
    // entry and in the same order. Out-of-range indices clamp.
    static float syncRateBeats (int index) noexcept
    {
        static const float beats[] = { 4.0f, 2.0f, 1.0f, 0.5f, 0.25f, 1.0f / 3.0f, 1.0f / 6.0f };
        static_assert (sizeof (beats) / sizeof (beats[0]) == (std::size_t) numSyncRates,
                       "syncRateNames and the beats table must stay the same length");
        const int n = (int) (sizeof (beats) / sizeof (beats[0]));
        return beats[fxme::jlimit (0, n - 1, index)];
    }

    // Choice lists for GUI combos / APVTS choice parameters. Plain string
    // literals plus a count rather than a framework string container, so this
    // header stays framework-free and allocates nothing. JUCE consumers build
    // the container they need directly from these:
    //
    //     StringArray (fxme::Lfo::shapeNames, fxme::Lfo::numShapes)   // JUCE's
    //
    // They live here, beside the beats tables they index into, so the two
    // cannot drift apart unnoticed — see the static_asserts below.
    static constexpr const char* const shapeNames[] = { "Sine", "Tri", "Square", "Saw Up", "Saw Dn" };
    static constexpr int numShapes = (int) (sizeof (shapeNames) / sizeof (shapeNames[0]));

    static constexpr const char* const syncRateNames[] = { "1/1", "1/2", "1/4", "1/8", "1/16", "1/8T", "1/16T" };
    static constexpr int numSyncRates = (int) (sizeof (syncRateNames) / sizeof (syncRateNames[0]));

    // ── Longer sync table, for modulation effects ────────────────────────────
    // syncRateNames above tops out at a whole note, which is far too fast
    // for a chorus or phaser sweep. This list runs from eight bars down to a
    // 1/32, then the triplets and the dotted values, and is returned in *beats
    // per cycle* (one beat = a quarter note) rather than as a note name, so a
    // consumer only ever multiplies by 60/bpm. Stored as an index in host
    // state, so the order only ever extends at the tail.
    static constexpr const char* const syncDivisionNames[] = {
        "8/1", "4/1", "2/1", "1/1", "1/2", "1/4", "1/8", "1/16", "1/32",
        "1/2T", "1/4T", "1/8T", "1/16T",
        "1/2.", "1/4.", "1/8.", "1/16." };
    static constexpr int numSyncDivisions = (int) (sizeof (syncDivisionNames) / sizeof (syncDivisionNames[0]));

    /** Index of "1/1" in syncDivisionNames — a sane default for a sweep. */
    static constexpr int defaultSyncDivision = 3;

    // Beats per cycle for a syncDivisionNames index. Out-of-range clamps.
    static float syncDivisionBeats (int index) noexcept
    {
        static const float beats[] = {
            32.0f, 16.0f, 8.0f, 4.0f, 2.0f, 1.0f, 0.5f, 0.25f, 0.125f,
            4.0f / 3.0f, 2.0f / 3.0f, 1.0f / 3.0f, 1.0f / 6.0f,
            3.0f, 1.5f, 0.75f, 0.375f
        };
        static_assert (sizeof (beats) / sizeof (beats[0]) == (std::size_t) numSyncDivisions,
                       "syncDivisionNames and the beats table must stay the same length");
        const int n = (int) (sizeof (beats) / sizeof (beats[0]));
        return beats[fxme::jlimit (0, n - 1, index)];
    }
};

} // namespace fxme
