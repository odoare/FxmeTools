/*
  ==============================================================================

    Scale.h

    Lightweight, dependency-free pitch-class scale engine for the FX-Mechanics
    tools. Scales are represented as 12-bit pitch-class masks (bit p set means
    pitch class p belongs to the scale), which makes membership tests and the
    "reverse lookup" used by the Neorix scale engine a couple of bit operations.

    This is a focused reimplementation of the subset of CppMusicTools::Scale that
    Neorix relies on, living in the `fxme` namespace so the project can depend on
    FxmeTools alone.

  ==============================================================================
*/

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace fxme
{

/** A pitch-class set over the 12 chromatic notes, stored as a 12-bit mask. */
using PitchClassSet = std::uint16_t;

/** The three scale families Neorix cross-references chords against. Kept as a
    small, ordered enum so callers can iterate over `ScaleType::Count`. */
enum class ScaleType
{
    Major = 0,        // Ionian
    HarmonicMinor,
    MelodicMinor,
    Count
};

/** Returns the chromatic interval recipe (semitone offsets from the root) for a
    scale family. */
inline const std::vector<int>& scaleIntervals (ScaleType type)
{
    static const std::vector<int> major         { 0, 2, 4, 5, 7, 9, 11 };
    static const std::vector<int> harmonicMinor { 0, 2, 3, 5, 7, 8, 11 };
    static const std::vector<int> melodicMinor  { 0, 2, 3, 5, 7, 9, 11 };

    switch (type)
    {
        case ScaleType::HarmonicMinor: return harmonicMinor;
        case ScaleType::MelodicMinor:  return melodicMinor;
        case ScaleType::Major:
        default:                       return major;
    }
}

/** Human-readable family name (without the root). */
inline const char* scaleTypeName (ScaleType type)
{
    switch (type)
    {
        case ScaleType::HarmonicMinor: return "Harmonic Minor";
        case ScaleType::MelodicMinor:  return "Melodic Minor";
        case ScaleType::Major:
        default:                       return "Major";
    }
}

/** Sharp note names indexed by pitch class. */
inline const char* pitchClassName (int pitchClass)
{
    static const std::array<const char*, 12> names
        { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return names[((pitchClass % 12) + 12) % 12];
}

/** Builds the 12-bit pitch-class mask for a scale rooted at `root` (0-11). */
inline PitchClassSet scaleMask (int root, ScaleType type)
{
    PitchClassSet mask = 0;
    const int r = ((root % 12) + 12) % 12;
    for (int interval : scaleIntervals (type))
        mask |= static_cast<PitchClassSet> (1u << ((r + interval) % 12));
    return mask;
}

/** True when every pitch class present in `chord` also belongs to `scale`. */
inline bool chordFitsInScale (PitchClassSet chord, PitchClassSet scale)
{
    return (chord & ~scale) == 0;
}

/** A concrete (root, family) pair plus its precomputed mask and display name. */
struct ScaleMatch
{
    int           root = 0;
    ScaleType     type = ScaleType::Major;
    PitchClassSet mask = 0;

    std::string name() const
    {
        return std::string (pitchClassName (root)) + " " + scaleTypeName (type);
    }
};

/** Returns every (root, family) scale, across all 12 roots and the supported
    families, that fully contains the given chord's pitch classes. */
inline std::vector<ScaleMatch> findMatchingScales (PitchClassSet chord)
{
    std::vector<ScaleMatch> matches;
    if (chord == 0)
        return matches;

    for (int t = 0; t < static_cast<int> (ScaleType::Count); ++t)
    {
        const auto type = static_cast<ScaleType> (t);
        for (int root = 0; root < 12; ++root)
        {
            const PitchClassSet mask = scaleMask (root, type);
            if (chordFitsInScale (chord, mask))
                matches.push_back ({ root, type, mask });
        }
    }
    return matches;
}

/** Convenience: collapse an arbitrary list of (possibly out-of-range) MIDI/pitch
    numbers into a pitch-class mask. */
template <typename Container>
inline PitchClassSet pitchClassSetFromNotes (const Container& notes)
{
    PitchClassSet mask = 0;
    for (int n : notes)
        mask |= static_cast<PitchClassSet> (1u << (((n % 12) + 12) % 12));
    return mask;
}

} // namespace fxme
