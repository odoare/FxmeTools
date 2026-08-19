/*
  ==============================================================================

    ChordName.h

    Lightweight, dependency-free chord detection for the FX-Mechanics tools.
    Given a pitch-class set (and, optionally, the sounding bass) it identifies
    the chord: root, quality suffix and — for inversions — a slash bass, so a
    set like {C,E,G} with bass E reads back as "C/E".

    Detection is a scored template match: every known chord shape is rotated to
    all twelve roots and compared against the set. Ties (symmetric chords such as
    dim7 / aug, or sus2 == sus4 a fifth apart) are resolved in favour of naming
    from the actual bass, which is what a player expects to read.

    Like Scale.h this lives in the `fxme` namespace and depends only on the
    standard library so a project can rely on FxmeTools alone.

  ==============================================================================
*/

#pragma once

#include <array>
#include <climits>
#include <cstdint>
#include <string>
#include <vector>

#include "Scale.h"   // PitchClassSet, pitchClassName

namespace fxme
{

/** The identified chord. `recognised` is false when the set matches no known
    template (e.g. an arbitrary cluster); callers can then fall back to listing
    the raw notes. */
struct ChordSymbol
{
    bool        recognised = false;
    int         root = -1;     // pitch class of the chord root
    int         bass = -1;     // pitch class of the bass (slash chord when != root)
    std::string quality;       // "", "m", "7", "maj7", "dim7", ...

    /** Display symbol, e.g. "C", "Dm7", "G7/B". Empty when unrecognised. */
    std::string text() const
    {
        if (! recognised || root < 0)
            return {};
        std::string s = pitchClassName (root);
        s += quality;
        if (bass >= 0 && bass != root)
        {
            s += "/";
            s += pitchClassName (bass);
        }
        return s;
    }
};

namespace chords
{
    /** A chord shape: semitone offsets from the root, a display suffix and a
        preference rank (lower = preferred when several shapes could fit). */
    struct Template
    {
        const char*      suffix;
        std::vector<int> intervals;
        int              rank;
    };

    /** The recognised shapes, from the common triads/sevenths the Harmonic-Table
        shapes can produce out to a few coloured chords. Extend freely. */
    inline const std::vector<Template>& templates()
    {
        static const std::vector<Template> t
        {
            // triads
            { "",      { 0, 4, 7 },     0 },
            { "m",     { 0, 3, 7 },     0 },
            { "dim",   { 0, 3, 6 },     2 },
            { "aug",   { 0, 4, 8 },     2 },
            { "sus4",  { 0, 5, 7 },     3 },
            { "sus2",  { 0, 2, 7 },     3 },
            // sevenths
            { "7",     { 0, 4, 7, 10 }, 0 },
            { "maj7",  { 0, 4, 7, 11 }, 0 },
            { "m7",    { 0, 3, 7, 10 }, 0 },
            { "mMaj7", { 0, 3, 7, 11 }, 1 },
            { "m7b5",  { 0, 3, 6, 10 }, 1 },
            { "dim7",  { 0, 3, 6, 9 },  1 },
            { "7b5",   { 0, 4, 6, 10 }, 2 },
            { "7#5",   { 0, 4, 8, 10 }, 2 },
            { "maj7#5",{ 0, 4, 8, 11 }, 2 },
            // sixths / added tones
            { "6",     { 0, 4, 7, 9 },  1 },
            { "m6",    { 0, 3, 7, 9 },  1 },
            { "add9",  { 0, 2, 4, 7 },  2 },
            { "madd9", { 0, 2, 3, 7 },  2 },
            { "7sus4", { 0, 5, 7, 10 }, 2 },
            // dyads
            { "5",     { 0, 7 },        3 },
        };
        return t;
    }

    /** Pitch-class mask for a template rooted at `root`. */
    inline PitchClassSet maskFrom (int root, const std::vector<int>& intervals)
    {
        PitchClassSet m = 0;
        for (int i : intervals)
            m |= static_cast<PitchClassSet> (1u << ((((root + i) % 12) + 12) % 12));
        return m;
    }

    inline int popcount (PitchClassSet pcs)
    {
        int n = 0;
        for (int p = 0; p < 12; ++p)
            if ((pcs >> p) & 1) ++n;
        return n;
    }
}

/** Identifies the chord described by a pitch-class set. Pass `bassPC` (0-11) when
    the sounding bass is known so inversions name correctly (slash chords) and
    symmetric chords are rooted on the bass. */
inline ChordSymbol detectChord (PitchClassSet pcs, int bassPC = -1)
{
    ChordSymbol best;
    const int count = chords::popcount (pcs);
    if (count == 0)
        return best;

    if (count == 1)
    {
        int p = 0;
        while (p < 12 && ! ((pcs >> p) & 1)) ++p;
        best.recognised = true;
        best.root = p;
        best.bass = p;            // single note: no slash
        return best;
    }

    int bestScore = INT_MAX;
    for (const auto& tpl : chords::templates())
    {
        if (static_cast<int> (tpl.intervals.size()) != count)
            continue;

        for (int root = 0; root < 12; ++root)
        {
            if (chords::maskFrom (root, tpl.intervals) != pcs)
                continue;

            // Primary: name from the bass when possible (so C/E/G/A over a C bass
            // reads "C6", not "Am7/C"). Secondary: the template's preference rank.
            int score = tpl.rank * 10;
            if (bassPC >= 0 && root != bassPC)
                score += 1000;

            if (score < bestScore)
            {
                bestScore = score;
                best.recognised = true;
                best.root = root;
                best.quality = tpl.suffix;
                best.bass = bassPC;
            }
        }
    }
    return best;
}

/** Note name with octave (C2 == MIDI 36, matching the engine's bass-octave
    convention), e.g. 60 -> "C4". */
inline std::string noteNameWithOctave (int midiNote)
{
    const int pc  = ((midiNote % 12) + 12) % 12;
    const int oct = midiNote / 12 - 1;
    return std::string (pitchClassName (pc)) + std::to_string (oct);
}

} // namespace fxme
