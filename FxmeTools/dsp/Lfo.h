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
            default:       return std::sin (juce::MathConstants<float>::twoPi * phase);
        }
    }

    // Beats per LFO cycle for a tempo-sync rate index, matching syncRateChoices()
    // ("1/1","1/2","1/4","1/8","1/16","1/8T","1/16T"). Out-of-range indices clamp.
    static float syncRateBeats (int index) noexcept
    {
        static const float beats[] = { 4.0f, 2.0f, 1.0f, 0.5f, 0.25f, 1.0f / 3.0f, 1.0f / 6.0f };
        const int n = (int) (sizeof (beats) / sizeof (beats[0]));
        return beats[juce::jlimit (0, n - 1, index)];
    }

    // Convenience choice lists for building GUI combos / APVTS choice parameters.
    static juce::StringArray shapeChoices()    { return { "Sine", "Tri", "Square", "Saw Up", "Saw Dn" }; }
    static juce::StringArray syncRateChoices() { return { "1/1", "1/2", "1/4", "1/8", "1/16", "1/8T", "1/16T" }; }
};

} // namespace fxme
