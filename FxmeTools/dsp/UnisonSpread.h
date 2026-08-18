/*
  ==============================================================================

    UnisonSpread.h

    Stateless spreading maths for a bank of detuned voices: how far each voice
    of a unison / ensemble / chorus is detuned, and where in a modulation cycle
    it sits. Pure functions of (voice index, voice count), with no state, no
    audio and no JUCE dependency, so the same distribution can be shared by the
    engine that renders the voices and the GUI that draws them.

    The two decisions worth having in one place, because both are easy to get
    subtly wrong:

      detuneFraction  Voices are spread evenly over [-1, +1], but assigned
                      *interleaved* — lowest, highest, second lowest, second
                      highest, and so on — so that neighbouring voice indices
                      never carry neighbouring pitches. A caller that places
                      its voices in index order (across the stereo field, or
                      around a sphere) would otherwise get a pitch gradient
                      running across the image rather than a crowd.

      phaseOffset     Successive voices are offset by the golden angle, as a
                      fraction of a turn, so their modulation never falls into
                      step for any voice count. Any rational fraction (a half,
                      a third) makes some counts move as one block; the golden
                      ratio conjugate is the standard irrational choice, and
                      is what makes the offsets stay spread as voices are
                      added and removed.

    Phase offsets are in cycles, matching fxme::Lfo and fxme::ModLfo, so a
    voice's modulation is lfo.valueAt (UnisonSpread::phaseOffset (voice)).

  ==============================================================================
*/

#pragma once

namespace fxme
{

class UnisonSpread
{
public:
    /** Detune of `voice` as a fraction of the maximum, in [-1, +1].

        Evenly spaced and interleaved (see the header note). A single voice is
        detuned fully up rather than left at zero, so that sweeping the voice
        count never silently changes what one voice does. */
    static float detuneFraction (int voice, int numVoices) noexcept
    {
        if (numVoices <= 1)
            return 1.0f;

        const int j = (voice % 2 == 0) ? voice / 2
                                       : numVoices - 1 - (voice - 1) / 2;
        return -1.0f + 2.0f * (float) j / (float) (numVoices - 1);
    }

    /** Detune of `voice` in cents, for a bank spanning [-maxCents, +maxCents]. */
    static float detuneCents (int voice, int numVoices, float maxCents) noexcept
    {
        return maxCents * detuneFraction (voice, numVoices);
    }

    /** Modulation phase offset of `voice`, in cycles (fractions of a turn, the
        fxme::Lfo convention), so the voices modulate out of step with each
        other. Deliberately independent of the voice count: adding a voice must
        not move the ones already sounding. */
    static float phaseOffset (int voice) noexcept
    {
        return (float) voice * goldenRatioConjugate;
    }

    /** 1 - 1/phi = 0.381966..., the golden angle as a fraction of a turn.

        The same angle as fxme::ambi::goldenAngle (2.3999632297 rad), which
        Ambisonics.h uses to build its spherical spiral, in the unit this class
        works in. Kept as its own literal rather than derived from that one, so
        that this header stays dependency-free; if either is ever changed, they
        are the same number and should move together. */
    static constexpr float goldenRatioConjugate = 0.38196601f;
};

} // namespace fxme
