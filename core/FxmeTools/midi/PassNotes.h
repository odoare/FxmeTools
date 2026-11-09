/*
  ------------------------------------------------------------------------------
    PassNotes.h

    Plays one event of a realized pass (PatternLanguage.h) through HeldNotes: the
    notes a link reaches go on, the others start, and the event's own link into
    the next event is set up. This is what every player of passes does, whatever
    its clock: an arpeggiator's step boundaries, a window of session beats, a
    conductor's hits.

    The host says what a note's pitch is, with a function

        float pitchOf (const pattern::Note& note, double date);   // < 0: none

    (a fractional pitch stays exact with MPE) and gives the date in beats on its clock.

    Real time: no allocation, no lock.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/midi/HeldNotes.h>
#include <FxmeTools/midi/PatternLanguage.h>

#include <array>

namespace fxme::pattern
{

static constexpr int maxEventNotes = 32;

/** The pitches an event plays at `date`: `hitPitch` for a hit `x` (< 0: a hit plays nothing),
    else its notes through `pitchOf`. How many. */
template <typename PitchOf>
int eventPitches (const Pass& pass, const Event& e, double date, float hitPitch, PitchOf&& pitchOf,
                  std::array<float, maxEventNotes>& out) noexcept
{
    if (e.noteCount == 0)
    {
        out[0] = hitPitch;
        return hitPitch >= 0.0f ? 1 : 0;
    }
    int found = 0;
    for (int n = 0; n < e.noteCount && n < maxEventNotes; ++n)
    {
        const float pitch = pitchOf (pass.notes[static_cast<std::size_t> (e.firstNote + n)], date);
        if (pitch >= 0.0f)
            out[static_cast<std::size_t> (found++)] = pitch;
    }
    return found;
}

/** How long the pass's first steps hold the sound the previous pass left (a hold with nothing
    before it, pattern-language §9.1): the notes that reach the pass's start ring that much longer
    (HeldNotes::extend). */
inline double tiedBeats (const Pass& pass) noexcept
{
    double tied = 0.0;
    for (const Step& s : pass.steps)
    {
        if (! s.tied)
            break;
        tied += s.duration;
    }
    return tied;
}

/** What playEvent() did. */
struct PlayedEvent
{
    int started = 0;           ///< notes attacked
    float lastPitch = -1.0f;   ///< the last of them; < 0: none
};

/** Plays `e`, an event of `pass`.
    @param date   when it falls, on the host's clock (a link's target is found by this date)
    @param start  when its notes are sent (`date`, or the block's start if it comes late)
    @param off    when it ends
    @param hitPitch what a hit `x` plays; < 0: nothing */
template <typename Out, typename PitchOf>
PlayedEvent playEvent (HeldNotes& held, MpeOut& mpe, Out& out, const Pass& pass, const Event& e, double date, double start, double off,
                       int velocity, float hitPitch, PitchOf&& pitchOf, HeldNotes::SameNote same = HeldNotes::SameNote::Cut) noexcept
{
    PlayedEvent played;
    std::array<float, maxEventNotes> pitches {};
    const int sounding = eventPitches (pass, e, date, hitPitch, pitchOf, pitches);

    // A link (~, ^) from the event before reaches this one: those notes go on; the others start.
    const auto left = held.arrive (out, mpe, date, off, ArrayView<float> (pitches.data(), sounding));
    for (int n = 0; n < sounding; ++n)
        if ((left & (1u << n)) != 0u)
        {
            const float pitch = pitches[static_cast<std::size_t> (n)];
            if (held.start (out, mpe, start, off, pitch, velocity, same))
            {
                ++played.started;
                played.lastPitch = pitch;
            }
        }

    // This one goes on into the next event (not across a rest: a note ends there).
    const auto index = static_cast<std::size_t> (&e - pass.events.data());
    if (e.link != Event::Link::None && ! e.linkAcrossRest && index + 1 < pass.events.size())
    {
        const Event& next = pass.events[index + 1];
        const double target = date + (next.onset - e.onset);
        std::array<float, maxEventNotes> to {};
        const int toCount = eventPitches (pass, next, target, hitPitch, pitchOf, to);
        held.link (start, e.link == Event::Link::Glide ? HeldNotes::Link::Glide : HeldNotes::Link::Legato,
                   date + e.linkStart, target, ArrayView<float> (to.data(), toCount));
    }
    return played;
}

} // namespace fxme::pattern
