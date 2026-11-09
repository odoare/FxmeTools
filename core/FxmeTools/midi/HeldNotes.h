/*
  ------------------------------------------------------------------------------
    HeldNotes.h

    The notes a source has sounding, when each ends, and how a note goes on into
    the next event: a glide `3~5` or a legato `3^5` of the pattern language. A
    linked note is not attacked again: with MPE it keeps its channel and its bend
    moves (a glide by points every 1/64 beat, a legato in one jump); without MPE
    the next note starts before this one ends, which a mono synth slurs.

    Dates are in beats, on any clock the host keeps (the session's, or one of its
    own); the output is the host's (see Mpe.h), which may turn beats into samples.

    Real time: fixed capacity, no allocation, no lock.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/midi/Mpe.h>
#include <FxmeTools/util/ArrayView.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace fxme
{

class HeldNotes
{
public:
    enum class Link { None, Glide, Legato };

    /** A note asked for while it already sounds: Cut ends the one sounding and attacks again;
        Keep lets it ring and plays nothing (an arpeggiator's habit). */
    enum class SameNote { Cut, Keep };

    static constexpr int capacity = 32;
    static constexpr double glideStep = 1.0 / 64.0;   ///< beats between two bend points of a glide

    /** A note starts at `at` and ends at `off`. False: it was not played (no room: its end would
        be lost; out of the MIDI range; or SameNote::Keep). */
    template <typename Out>
    bool start (Out& out, MpeOut& mpe, double at, double off, float pitch, int velocity, SameNote same = SameNote::Cut) noexcept
    {
        const long note = std::lround (pitch);
        if (note < 0 || note > 127)
            return false;
        for (int i = 0; i < count;)
        {
            const Held& h = at_ (i);
            if (h.out.note == note && (mpe.mpe() || h.out.channel == mpe.channel()))
            {
                if (same == SameNote::Keep)
                    return false;
                end (out, mpe, i, at);
            }
            else
                ++i;
        }
        if (count >= capacity)
            return false;
        const auto n = mpe.noteOn (out, at, pitch, velocity);
        if (! n)
            return false;
        Held h;
        h.out = n;
        h.pitch = pitch;
        h.velocity = velocity;
        h.started = at;
        h.off = off;
        held[static_cast<std::size_t> (count++)] = h;
        return true;
    }

    /** The notes started at `onset` go on into the event at `target` (`pitches`, paired with
        them by pitch: in order when as many, else each target to the nearest note). A glide
        starts moving at `glideFrom`. They no longer end before `target`. A note left without a
        pitch ends as written. */
    void link (double onset, Link kind, double glideFrom, double target, ArrayView<float> pitches) noexcept
    {
        std::array<int, capacity> sources {}, pairOf {};
        int n = 0;
        for (int i = 0; i < count; ++i)
            if (std::fabs (at_ (i).started - onset) < sameDate)
                sources[static_cast<std::size_t> (n++)] = i;
        pair (sources.data(), n, pitches, pairOf.data());
        for (int k = 0; k < n; ++k)
        {
            const int to = pairOf[static_cast<std::size_t> (k)];
            if (to < 0)
                continue;
            Held& h = at_ (sources[static_cast<std::size_t> (k)]);
            h.link = kind;
            h.glideFrom = std::min (std::max (glideFrom, onset), target);
            h.target = target;
            h.toPitch = pitches[to];
            h.off = std::max (h.off, target + sameDate);   // not released before the target arrives
        }
    }

    /** The event at `at` that a link reaches, ending at `off`: its pitches continue the linked
        notes (paired as link() did), which then link no more. Linked notes left without a pitch
        end at `at`. Returns the pitches nothing continued, one bit each: the caller starts them. */
    template <typename Out>
    std::uint32_t arrive (Out& out, MpeOut& mpe, double at, double off, ArrayView<float> pitches) noexcept
    {
        const int total = std::min (pitches.size(), 32);
        std::uint32_t left = total >= 32 ? 0xFFFFFFFFu : (1u << total) - 1u;
        std::array<int, capacity> sources {}, pairOf {};
        int n = 0;
        for (int i = 0; i < count; ++i)
            if (at_ (i).link != Link::None && std::fabs (at_ (i).target - at) < sameDate)
                sources[static_cast<std::size_t> (n++)] = i;
        if (n == 0)
            return left;
        pair (sources.data(), n, pitches, pairOf.data());

        // Backwards: ending a note moves the ones after it down.
        for (int k = n - 1; k >= 0; --k)
        {
            const int i = sources[static_cast<std::size_t> (k)];
            const int to = pairOf[static_cast<std::size_t> (k)];
            Held& h = at_ (i);
            h.link = Link::None;
            if (to < 0)
            {
                end (out, mpe, i, at);
                continue;
            }
            const float pitch = pitches[to];
            left &= ~(1u << to);
            if (! mpe.bend (out, at, h.out, pitch))
            {
                // No MPE (or too far for a bend): the next note before this one ends, a slur on a mono synth.
                const auto next = mpe.noteOn (out, at, pitch, h.velocity);
                mpe.noteOff (out, at, h.out);
                if (! next)
                {
                    remove (i);
                    continue;
                }
                h.out = next;
            }
            h.pitch = pitch;
            h.started = at;
            h.off = off;
        }
        return left;
    }

    /** The glides' bend points in [from, to). */
    template <typename Out>
    void glide (Out& out, MpeOut& mpe, double from, double to) noexcept
    {
        if (! mpe.mpe())
            return;
        for (int i = 0; i < count; ++i)
        {
            const Held& h = at_ (i);
            if (h.link != Link::Glide || h.target <= h.glideFrom)
                continue;
            const double begin = std::max (from, h.glideFrom);
            const double finish = std::min (to, h.target);
            // Points on a grid of glideStep from the glide's start, so every block agrees.
            for (double t = h.glideFrom + std::ceil ((begin - h.glideFrom) / glideStep - 1e-9) * glideStep; t < finish; t += glideStep)
            {
                const auto k = static_cast<float> ((t - h.glideFrom) / (h.target - h.glideFrom));
                mpe.bend (out, t, h.out, h.pitch + (h.toPitch - h.pitch) * k);
            }
        }
    }

    /** Ends the notes whose end is before `beat` (or at it, `inclusive`), dated at their end but
        never before `notBefore` (the block's start). */
    template <typename Out>
    void releaseUpTo (Out& out, MpeOut& mpe, double beat, bool inclusive, double notBefore) noexcept
    {
        for (int i = 0; i < count;)
        {
            const double off = at_ (i).off;
            if (inclusive ? off <= beat + 1e-9 : off < beat)
                end (out, mpe, i, std::max (off, notBefore));
            else
                ++i;
        }
    }

    /** Ends the notes whose end is at `beat` or before, with no tolerance (a host with its own),
        dated `beat`. How many. */
    template <typename Out>
    int releaseDue (Out& out, MpeOut& mpe, double beat) noexcept
    {
        int ended = 0;
        for (int i = 0; i < count;)
        {
            if (at_ (i).off <= beat)
            {
                end (out, mpe, i, beat);
                ++ended;
            }
            else
                ++i;
        }
        return ended;
    }

    /** Everything ends now. */
    template <typename Out>
    void silence (Out& out, MpeOut& mpe, double beat) noexcept
    {
        while (count > 0)
            end (out, mpe, 0, beat);
    }

    /** The notes that reach `from` ring `by` beats longer (a tie across passes). */
    void extend (double from, double by) noexcept
    {
        for (int i = 0; i < count; ++i)
            if (at_ (i).off >= from)
                at_ (i).off += by;
    }

    /** When the next note ends; infinity if none sounds. */
    double nextEnd() const noexcept
    {
        double next = std::numeric_limits<double>::infinity();
        for (int i = 0; i < count; ++i)
            next = std::min (next, held[static_cast<std::size_t> (i)].off);
        return next;
    }

    int sounding() const noexcept { return count; }

    /** Forgets everything, without a note-off (before playing starts). */
    void clear() noexcept { count = 0; }

private:
    static constexpr double sameDate = 1e-6;   // two dates this close are one

    struct Held
    {
        MpeOut::Note out;
        float pitch = 0.0f;               ///< sounding now (its last arrival)
        int velocity = 100;
        double started = 0.0;             ///< its onset, or its last arrival
        double off = std::numeric_limits<double>::infinity();
        Link link = Link::None;
        double glideFrom = 0.0, target = -1.0;
        float toPitch = 0.0f;
    };

    Held& at_ (int i) noexcept { return held[static_cast<std::size_t> (i)]; }
    const Held& at_ (int i) const noexcept { return held[static_cast<std::size_t> (i)]; }

    // Compact, in the order they started: notes that end together end in that order.
    void remove (int i) noexcept
    {
        for (int j = i + 1; j < count; ++j)
            at_ (j - 1) = at_ (j);
        --count;
    }

    template <typename Out>
    void end (Out& out, MpeOut& mpe, int i, double time) noexcept
    {
        mpe.noteOff (out, time, at_ (i).out);
        remove (i);
    }

    /** Pairs `sources` (indices into held) with `pitches`: pairOf[k] = the pitch for sources[k], -1 none. */
    void pair (const int* sources, int n, ArrayView<float> pitches, int* pairOf) const noexcept
    {
        std::fill (pairOf, pairOf + n, -1);
        // Both sides by pitch, lowest first (insertion sorts: a few notes, no allocation).
        std::array<int, capacity> src {}, dst {};
        const int nd = std::min (pitches.size(), capacity);
        for (int k = 0; k < n; ++k)
        {
            int j = k;
            for (; j > 0 && at_ (sources[k]).pitch < at_ (sources[src[static_cast<std::size_t> (j - 1)]]).pitch; --j)
                src[static_cast<std::size_t> (j)] = src[static_cast<std::size_t> (j - 1)];
            src[static_cast<std::size_t> (j)] = k;
        }
        for (int k = 0; k < nd; ++k)
        {
            int j = k;
            for (; j > 0 && pitches[k] < pitches[dst[static_cast<std::size_t> (j - 1)]]; --j)
                dst[static_cast<std::size_t> (j)] = dst[static_cast<std::size_t> (j - 1)];
            dst[static_cast<std::size_t> (j)] = k;
        }

        if (n == nd)
        {
            for (int k = 0; k < n; ++k)
                pairOf[src[static_cast<std::size_t> (k)]] = dst[static_cast<std::size_t> (k)];
            return;
        }
        // Not as many: each target to the nearest note still free, lowest target first.
        std::array<bool, capacity> used {};
        for (int t = 0; t < nd; ++t)
        {
            const int target = dst[static_cast<std::size_t> (t)];
            const float p = pitches[target];
            int best = -1;
            for (int k = 0; k < n; ++k)
                if (! used[static_cast<std::size_t> (k)]
                    && (best < 0 || std::fabs (at_ (sources[k]).pitch - p) < std::fabs (at_ (sources[best]).pitch - p)))
                    best = k;
            if (best < 0)
                break;
            used[static_cast<std::size_t> (best)] = true;
            pairOf[best] = target;
        }
    }

    std::array<Held, capacity> held {};
    int count = 0;
};

} // namespace fxme
