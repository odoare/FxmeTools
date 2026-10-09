/*
  ------------------------------------------------------------------------------
    synth/ModMatrix.h

    The arithmetic of a modulation matrix, independent of what the sources and
    destinations are. A route takes one source value, scales it by a bipolar
    depth and optionally by a second "via" source (a mod wheel scaling an LFO's
    depth, say), and adds the result to one destination's offset. Offsets are
    in the destination's normalised [0, 1] space, so a depth of 1 sweeps the
    whole range of any parameter and the owner turns base + offset back into
    a value with the parameter's own mapping (applyOffset does the clamp).

    Source and destination are plain indices into the owner's arrays; index 0
    of the sources is "none" by convention and is never read.

    Also here: RouteSet, a fixed array of routes the audio thread reads while
    another thread edits it, each field an atomic so that no route is ever
    read torn into an invalid index (a route is at worst seen half old and
    half new for one block).

    Header-only, no allocation, realtime safe.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace fxme
{

struct ModRoute
{
    int source = 0;        // 0: none
    int destination = -1;  // -1: none
    int via = 0;           // 0: no via source
    float depth = 0.0f;    // -1 .. 1

    bool isActive() const noexcept { return source > 0 && destination >= 0 && (depth > 0.0f || depth < 0.0f); }
};

namespace ModMatrix
{
    /** The amount route `r` adds to its destination, given the source values. */
    inline float routeAmount (const ModRoute& r, const float* sources) noexcept
    {
        float v = sources[r.source] * r.depth;
        if (r.via > 0)
            v *= sources[r.via];
        return v;
    }

    /** Base (normalised) plus offset, clamped to [0, 1]. */
    inline float applyOffset (float base01, float offset) noexcept
    {
        const float v = base01 + offset;
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }
}

//==============================================================================
template <int MaxRoutes>
class RouteSet
{
public:
    static constexpr int size = MaxRoutes;

    /** Writer side (message thread). */
    void set (int index, int source, int destination, int via) noexcept
    {
        auto& r = routes[(std::size_t) index];
        r.source.store (source, std::memory_order_relaxed);
        r.destination.store (destination, std::memory_order_relaxed);
        r.via.store (via, std::memory_order_relaxed);
    }

    void clear (int index) noexcept { set (index, 0, -1, 0); }

    /** Reader side (audio thread): the route's indices, depth left to the
        caller (usually an automatable parameter read separately). */
    ModRoute get (int index) const noexcept
    {
        const auto& r = routes[(std::size_t) index];
        ModRoute out;
        out.source = r.source.load (std::memory_order_relaxed);
        out.destination = r.destination.load (std::memory_order_relaxed);
        out.via = r.via.load (std::memory_order_relaxed);
        return out;
    }

private:
    struct AtomicRoute
    {
        std::atomic<int> source { 0 }, destination { -1 }, via { 0 };
    };
    std::array<AtomicRoute, (std::size_t) MaxRoutes> routes {};
};

} // namespace fxme
