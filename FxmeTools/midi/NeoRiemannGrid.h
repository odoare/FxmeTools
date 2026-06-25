/*
  ==============================================================================

    NeoRiemannGrid.h

    A dependency-free model of an isomorphic "Harmonic Table" hexagonal grid and
    the three sub-hexagon selection geometries (Face / Spoke / Border) used by
    Neorix to extract 4-note chords. The model knows nothing about JUCE or
    drawing: it deals in axial hex coordinates, plain 2-D points for pixel
    layout, and pitch classes. A host component layers Graphics + mouse on top.

    Harmonic Table layout (flat-top hexagons, fifths run vertically):

        pitch(q, r) = root - 3*q - 7*r   (in semitones, mod 12 for pitch class)

    so the six neighbours of any hex are at intervals
        N = +7  S = -7   (perfect fifth)
        NE = +4 SW = -4   (major third)
        NW = +3 SE = -3   (minor third)
    and any three mutually-adjacent hexes form a major or minor triad.

  ==============================================================================
*/

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace fxme
{

/** Axial hexagon coordinate. */
struct Hex
{
    int q = 0;
    int r = 0;
};

inline bool operator== (Hex a, Hex b) { return a.q == b.q && a.r == b.r; }
inline bool operator!= (Hex a, Hex b) { return ! (a == b); }
inline Hex  operator+  (Hex a, Hex b) { return { a.q + b.q, a.r + b.r }; }

/** A simple 2-D point used for pixel layout (avoids a JUCE dependency here). */
struct Vec2 { float x = 0.0f; float y = 0.0f; };

/** The interactive sub-hexagon geometries.
      Center  - the hexagon's centre (its letter): a single note.
      Point   - a hexagon vertex, where three hexes meet: a Tonnetz triad.
      Face    - an inner triangle (slice): a 4-note cluster.
      Spoke   - an inner segment (centre -> vertex): 4 notes.
      Border  - an outer side (edge): the symmetric axis chord. */
enum class GridMode { Center, Point, Face, Spoke, Border };

/** A geometric selection on the grid: a mode, the originating hex and the
    edge/corner index (0-5) it applies to. */
struct Selection
{
    GridMode mode = GridMode::Face;
    Hex      hex;        // for Border this is hex "A"; the partner is hex+edgeDir
    int      index = 0;  // edge index (Face/Border) or corner index (Spoke)

    bool operator== (const Selection& o) const
    {
        return mode == o.mode && hex == o.hex && index == o.index;
    }
    bool operator!= (const Selection& o) const { return ! (*this == o); }
};

/** The four hexes captured by a selection, and which of them is the bass. */
struct HexChord
{
    std::array<Hex, 4> hexes {};
    int                bassIndex = 0;
};

namespace grid
{
    constexpr float kSqrt3 = 1.7320508075688772f;

    /** Edge/corner directions in cyclic (increasing screen-angle) order, so
        adjacent entries share a hex corner. Index i is the neighbour across
        edge i; corner i sits between edge i-1 and edge i. */
    inline const std::array<Hex, 6>& edgeDirs()
    {
        static const std::array<Hex, 6> dirs
            { Hex{1,0}, Hex{0,1}, Hex{-1,1}, Hex{-1,0}, Hex{0,-1}, Hex{1,-1} };
        return dirs;
    }

    /** Linear pitch offset (semitones) of a hex relative to the grid root. */
    inline int pitchOffset (Hex h) { return -3 * h.q - 7 * h.r; }

    /** Pitch class (0-11) of a hex given the grid root pitch class. */
    inline int pitchClass (Hex h, int rootPitchClass)
    {
        return (((rootPitchClass + pitchOffset (h)) % 12) + 12) % 12;
    }

    // ---- pixel layout (flat-top hexagons) ----------------------------------

    inline Vec2 hexCenter (Hex h, float size, Vec2 origin)
    {
        const float x = size * 1.5f * static_cast<float> (h.q);
        const float y = size * kSqrt3 * (static_cast<float> (h.r) + 0.5f * static_cast<float> (h.q));
        return { origin.x + x, origin.y + y };
    }

    inline Vec2 hexCorner (Vec2 center, float size, int corner)
    {
        const float a = 3.14159265358979f / 180.0f * (60.0f * static_cast<float> (corner));
        return { center.x + size * std::cos (a), center.y + size * std::sin (a) };
    }

    /** Rounds fractional axial coordinates to the nearest hex (cube rounding). */
    inline Hex axialRound (float qf, float rf)
    {
        const float xf = qf;
        const float zf = rf;
        const float yf = -xf - zf;

        float rx = std::round (xf);
        float ry = std::round (yf);
        float rz = std::round (zf);

        const float dx = std::abs (rx - xf);
        const float dy = std::abs (ry - yf);
        const float dz = std::abs (rz - zf);

        if (dx > dy && dx > dz)      rx = -ry - rz;
        else if (dy > dz)            ry = -rx - rz;
        else                         rz = -rx - ry;

        return { static_cast<int> (rx), static_cast<int> (rz) };
    }

    /** Converts a pixel (relative to the same origin used by hexCenter) to the
        hex whose centre is nearest. */
    inline Hex pixelToHex (Vec2 p, float size, Vec2 origin)
    {
        const float px = p.x - origin.x;
        const float py = p.y - origin.y;
        const float qf = (px / size) / 1.5f;
        const float rf = (py / (size * kSqrt3)) - 0.5f * qf;
        return axialRound (qf, rf);
    }

    // ---- chord extraction --------------------------------------------------

    /** The distinct hexagons a selection involves (its bass first). Used both as
        the source of the chord's pitch classes and to highlight the grid letters
        the shape will sound. */
    inline std::vector<Hex> selectionHexes (const Selection& s)
    {
        const auto& d = edgeDirs();
        const Hex x = s.hex;
        const int i = s.index;

        switch (s.mode)
        {
            case GridMode::Center:
                return { x };

            case GridMode::Point:
            {
                // Corner `index` spans edges index-1 and index (hitTest numbering).
                // The clicked hex + the two neighbours sharing that vertex form a
                // major/minor Tonnetz triad.
                return { x, x + d[(i + 5) % 6], x + d[i] };
            }

            case GridMode::Face:
                return { x, x + d[i], x + d[(i + 5) % 6], x + d[(i + 1) % 6] };

            case GridMode::Spoke:
            {
                const Hex d1 = d[(i + 5) % 6];
                const Hex d2 = d[i];
                return { x, x + d1, x + d2, x + d1 + d2 };
            }

            case GridMode::Border:
            default:
            {
                // The symmetric chord of the edge's interval axis, rooted at x:
                //   fifth (±7) -> column x, x±N, x+2S    -> {0,5,7,10}
                //   maj-3 (±4) -> x, x+NE, x+SW          -> {0,4,8}
                //   min-3 (±3) -> x, x+NW, x+SE, x+2SE   -> {0,3,6,9}
                const int axis = std::abs (pitchOffset (d[i % 6])) % 12;
                const Hex N { 0, -1 }, S { 0, 1 }, NE { 1, -1 }, SW { -1, 1 }, NW { -1, 0 }, SE { 1, 0 };
                if (axis == 4 || axis == 8)               // major-third axis
                    return { x, x + NE, x + SW };
                if (axis == 3 || axis == 9)               // minor-third axis
                    return { x, x + NW, x + SE, x + SE + SE };
                return { x, x + N, x + S, x + S + S };     // perfect-fifth axis
            }
        }
    }

    /** Resolves a selection into four hexes + bass index (bass is always the
        clicked hexagon). The hexes carry the chord's pitch classes; a shape with
        fewer than four notes simply repeats one. */
    inline HexChord resolve (const Selection& s)
    {
        const std::vector<Hex> hs = selectionHexes (s);
        HexChord c;
        c.bassIndex = 0;
        for (int i = 0; i < 4; ++i)
            c.hexes[i] = hs[std::min<size_t> ((size_t) i, hs.size() - 1)];
        return c;
    }

    /** Pitch classes of a selection's notes (padded to four; duplicates collapse
        later in voiceChord / chordMask). */
    inline std::array<int, 4> chordPitchClasses (const Selection& s, int rootPitchClass)
    {
        const HexChord c = resolve (s);
        std::array<int, 4> pcs {};
        for (int i = 0; i < 4; ++i)
            pcs[i] = pitchClass (c.hexes[i], rootPitchClass);
        return pcs;
    }

    /** 12-bit pitch-class mask of a selection (for scale tests). */
    inline std::uint16_t chordMask (const Selection& s, int rootPitchClass)
    {
        std::uint16_t m = 0;
        for (int pc : chordPitchClasses (s, rootPitchClass))
            m |= static_cast<std::uint16_t> (1u << pc);
        return m;
    }

    /** MIDI-engine voicing (spec section 4): the bass pitch class is anchored at
        octave N (bassC = MIDI number of C in that octave), and the remaining
        notes are wrapped into the single octave above the bass, de-duplicated and
        sorted ascending.

        @param pcs        the four pitch classes
        @param bassIndex  which of the four is the bass
        @param bassC      MIDI note of C in the bass octave (e.g. 36 for C2)
        @returns          ascending MIDI note numbers (bass first), 1..4 notes. */
    inline std::vector<int> voiceChord (const std::array<int, 4>& pcs, int bassIndex, int bassC)
    {
        const int bassPC  = pcs[bassIndex];
        const int bassMidi = bassC + bassPC;

        std::vector<int> upper;
        upper.reserve (3);
        for (int i = 0; i < 4; ++i)
        {
            if (i == bassIndex)
                continue;
            const int interval = ((pcs[i] - bassPC) % 12 + 12) % 12;
            upper.push_back (bassMidi + (interval == 0 ? 12 : interval));
        }

        std::sort (upper.begin(), upper.end());
        upper.erase (std::unique (upper.begin(), upper.end()), upper.end());

        std::vector<int> notes;
        notes.reserve (4);
        notes.push_back (bassMidi);
        for (int n : upper)
            notes.push_back (n);
        return notes;
    }

    /** Final MIDI notes for a selection, ready to play. Center yields a single
        note; every other mode goes through the octave-wrapping voicer. */
    inline std::vector<int> selectionMidiNotes (const Selection& s, int rootPitchClass, int bassC)
    {
        if (s.mode == GridMode::Center)
            return { bassC + pitchClass (s.hex, rootPitchClass) };

        const HexChord c = resolve (s);
        return voiceChord (chordPitchClasses (s, rootPitchClass), c.bassIndex, bassC);
    }

    /** Enumerates every distinct sub-geometry over a rectangular block of hexes.
        Borders are emitted once (only for the three "forward" edge directions)
        so a shared edge is not produced twice. */
    inline std::vector<Selection> enumerateSelections (const std::vector<Hex>& hexes)
    {
        std::vector<Selection> out;
        out.reserve (hexes.size() * 15);
        for (const Hex& h : hexes)
        {
            out.push_back ({ GridMode::Center, h, 0 });
            for (int i = 0; i < 6; ++i)
            {
                out.push_back ({ GridMode::Point, h, i });
                out.push_back ({ GridMode::Face,  h, i });
                out.push_back ({ GridMode::Spoke, h, i });
                if (i < 3)  // forward edges only -> each border counted once
                    out.push_back ({ GridMode::Border, h, i });
            }
        }
        return out;
    }

    // ---- mouse hit-testing -------------------------------------------------

    /** Given a pixel, returns the selection (mode + edge/corner) the user is
        pointing at. Hexes are split into six angular sectors (the six faces);
        within a sector the radial position and proximity to a corner ray decide
        between Face (interior), Spoke (near a corner ray) and Border (near the
        outer edge). */
    inline Selection hitTest (Vec2 p, float size, Vec2 origin)
    {
        const Hex  h = pixelToHex (p, size, origin);
        const Vec2 c = hexCenter (h, size, origin);

        const float lx = p.x - c.x;
        const float ly = p.y - c.y;
        const float dist = std::sqrt (lx * lx + ly * ly);

        float ang = std::atan2 (ly, lx) * 180.0f / 3.14159265358979f; // [-180,180]
        if (ang < 0.0f) ang += 360.0f;                                // [0,360)

        const int edgeIndex   = static_cast<int> (std::floor (ang / 60.0f)) % 6;
        const int nearCorner  = static_cast<int> (std::lround (ang / 60.0f)) % 6;

        // Distance from the centre to the hex outline along this angle: the edge
        // midpoint of edge `edgeIndex` sits at angle 60*edgeIndex + 30.
        const float apothem = size * kSqrt3 * 0.5f;
        const float edgeAngle = (60.0f * edgeIndex + 30.0f) * 3.14159265358979f / 180.0f;
        const float pointAngle = ang * 3.14159265358979f / 180.0f;
        const float edgeDist = apothem / std::max (0.2f, std::cos (pointAngle - edgeAngle));
        const float f = (edgeDist > 0.0f) ? (dist / edgeDist) : 1.0f;

        // angular distance to the nearest corner ray
        float cornerDelta = std::abs (ang - 60.0f * nearCorner);
        if (cornerDelta > 180.0f) cornerDelta = 360.0f - cornerDelta;

        constexpr float centreThreshold = 0.22f;   // inner disc  -> Center (letter)
        constexpr float pointThreshold   = 0.68f;   // near corner tip -> Point (enlarged)
        constexpr float pointAngleMax    = 23.0f;
        constexpr float borderThreshold  = 0.78f;   // near edge midpoint -> Border (enlarged)
        constexpr float spokeAngle       = 18.0f;   // segment ray (enlarged)
        constexpr float spokeInner       = 0.22f;

        if (f < centreThreshold)
            return { GridMode::Center, h, 0 };

        if (cornerDelta < pointAngleMax && f > pointThreshold)
            return { GridMode::Point, h, nearCorner };

        if (f > borderThreshold)
            return { GridMode::Border, h, edgeIndex };

        if (cornerDelta < spokeAngle && f > spokeInner)
            return { GridMode::Spoke, h, nearCorner };

        return { GridMode::Face, h, edgeIndex };
    }

} // namespace grid
} // namespace fxme
