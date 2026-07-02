/*
  ==============================================================================

    GridTransform.h

    A generic navigation algebra for the Harmonic Table (NeoRiemannGrid.h).

    Every non-Center shape of a hexagon lives on one of two 12-slot rings:

      inner ring:  … spoke(i) → face(i) → spoke(i+1) → face(i+1) …
      outer ring:  … point(i) → border(i) → point(i+1) → border(i+1) …

    encoded as a canonical ring coordinate

      (hex, outer, slot, modifier)
        outer ∈ {false = spoke/face, true = point/border}
        slot  ∈ 0..11:  slot 2i   = spoke(i) / point(i)
                        slot 2i+1 = face(i)  / border(i)

    The slot phase is chosen so the natural depth pairing preserves the slot:
    spoke(i) ↔ point(i) and face(i) ↔ border(i) sit at the same slot on the
    two rings — going inward/outward is a level flip with the slot kept.

    A GridTransform moves one component per axis (hex, slot, level, modifier),
    each either kept, offset, or set to an absolute value. Hex moves can also
    follow the edge direction of the current sector (slot / 2), which is what
    cross-hex moves such as "extend outward across the border" need.

    A TransformRule guards a transform on the current level and/or slot parity;
    a KeyBinding is an ordered rule list — the first matching rule applies.

  ==============================================================================
*/

#pragma once

#include <optional>
#include <vector>
#include "NeoRiemannGrid.h"

namespace fxme
{

// ---- Ring coordinates -------------------------------------------------------

/** Canonical ring coordinate of a non-Center selection. */
struct RingPos
{
    Hex  hex;
    bool outer    = false;  // false = spoke/face ring, true = point/border ring
    int  slot     = 0;      // 0..11, see file header for the phase convention
    int  modifier = 0;      // 0..3
};

/** Selection → ring coordinate. Center has no ring position. */
inline std::optional<RingPos> toRing (const Selection& s)
{
    switch (s.mode)
    {
        case GridMode::Spoke:  return RingPos { s.hex, false, 2 * s.index,     s.modifier };
        case GridMode::Face:   return RingPos { s.hex, false, 2 * s.index + 1, s.modifier };
        case GridMode::Point:  return RingPos { s.hex, true,  2 * s.index,     s.modifier };
        case GridMode::Border: return RingPos { s.hex, true,  2 * s.index + 1, s.modifier };
        default:               return std::nullopt;
    }
}

/** Ring coordinate → Selection. */
inline Selection fromRing (const RingPos& r)
{
    const int  index = r.slot / 2;
    const bool odd   = (r.slot & 1) != 0;
    const GridMode mode = r.outer ? (odd ? GridMode::Border : GridMode::Point)
                                  : (odd ? GridMode::Face   : GridMode::Spoke);
    return Selection { mode, r.hex, index, r.modifier };
}

// ---- Transform components ---------------------------------------------------

/** One cyclic component of a transform: keep, offset by n, or set to n. */
struct Coord
{
    enum class Kind { Keep, Offset, Set };

    Kind kind  = Kind::Keep;
    int  value = 0;

    static constexpr Coord keep()          { return {}; }
    static constexpr Coord offset (int n)  { return { Kind::Offset, n }; }
    static constexpr Coord set (int n)     { return { Kind::Set,    n }; }

    int apply (int current, int modulus) const
    {
        int v = current;
        switch (kind)
        {
            case Kind::Keep:   break;
            case Kind::Offset: v = current + value; break;
            case Kind::Set:    v = value;           break;
        }
        return ((v % modulus) + modulus) % modulus;
    }
};

/** Hex component of a transform. SectorDir moves along the edge direction of
    the current sector (slot / 2) plus an offset — the primitive cross-hex
    moves need, since their direction depends on where the shape sits. */
struct HexMove
{
    enum class Kind { Keep, Relative, Absolute, SectorDir };

    Kind kind = Kind::Keep;
    int  q = 0;   // Relative / Absolute: axial q.  SectorDir: sector offset.
    int  r = 0;   // Relative / Absolute: axial r.  SectorDir: unused.

    static constexpr HexMove keep()                    { return {}; }
    static constexpr HexMove relative (int dq, int dr) { return { Kind::Relative, dq, dr }; }
    static constexpr HexMove absolute (int q, int r)   { return { Kind::Absolute, q, r }; }
    static constexpr HexMove sectorDir (int offset = 0){ return { Kind::SectorDir, offset, 0 }; }

    Hex apply (Hex current, int sector) const
    {
        switch (kind)
        {
            case Kind::Keep:      return current;
            case Kind::Relative:  return { current.q + q, current.r + r };
            case Kind::Absolute:  return { q, r };
            case Kind::SectorDir: return current + grid::edgeDirs()[(((sector + q) % 6) + 6) % 6];
        }
        return current;
    }
};

/** Level (ring depth) component of a transform. */
enum class LevelOp { Keep, Toggle, SetInner, SetOuter };

// ---- GridTransform ------------------------------------------------------------

/** A component-wise move in ring space. The sector used by HexMove::SectorDir
    is taken from the position *before* the slot component is applied. */
struct GridTransform
{
    HexMove hex;
    Coord   slot;                    // applied mod 12
    LevelOp level = LevelOp::Keep;
    Coord   modifier;                // applied mod 4

    std::optional<Selection> apply (const Selection& s) const
    {
        const auto rp = toRing (s);
        if (! rp)
            return std::nullopt;

        RingPos out = *rp;
        out.hex      = hex.apply (rp->hex, rp->slot / 2);
        out.slot     = slot.apply (rp->slot, 12);
        out.modifier = modifier.apply (rp->modifier, 4);
        switch (level)
        {
            case LevelOp::Keep:     break;
            case LevelOp::Toggle:   out.outer = ! rp->outer; break;
            case LevelOp::SetInner: out.outer = false;       break;
            case LevelOp::SetOuter: out.outer = true;        break;
        }
        return fromRing (out);
    }
};

// ---- Guarded rules ------------------------------------------------------------

/** A transform guarded on the current ring position. Empty guards match all. */
struct TransformRule
{
    std::optional<bool> requireOuter;   // match only inner (false) / outer (true)
    std::optional<int>  requireParity;  // 0 = spoke/point, 1 = face/border
    GridTransform       transform;

    bool matches (const RingPos& r) const
    {
        if (requireOuter  && *requireOuter  != r.outer)      return false;
        if (requireParity && *requireParity != (r.slot & 1)) return false;
        return true;
    }
};

/** An ordered rule list bound to one key / action: first matching rule wins. */
using KeyBinding = std::vector<TransformRule>;

/** Apply a binding to a selection. Returns nullopt when the selection is a
    Center or no rule matches. */
inline std::optional<Selection> applyBinding (const Selection& s, const KeyBinding& binding)
{
    const auto rp = toRing (s);
    if (! rp)
        return std::nullopt;

    for (const auto& rule : binding)
        if (rule.matches (*rp))
            return rule.transform.apply (s);

    return std::nullopt;
}

} // namespace fxme
