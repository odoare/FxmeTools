/*
  ------------------------------------------------------------------------------
    DelayTimeResolver.h

    Turns one delay-time parameter into seconds three different ways, so a
    plugin can offer free, tempo-locked and pitch-locked delays without three
    parameters per delay. The stored value is always the same number; only how
    it is read changes:

      seconds     the value is the delay, in seconds
      tempoSync   the value is a proportion of a whole note, so 0.25 is a
                  quarter note, 1/3 is a whole-note triplet, and any value in
                  between is a length no note name covers
      notePeriod  the value multiplies the period of a pitch (typically the
                  last MIDI note in). At a coefficient of 1 the delay is one
                  cycle long and the line becomes a resonator tuned to that
                  note; small integer multiples drop it by octaves

    Expressing sync as a fraction rather than a list of note values is what
    makes the three modes share one parameter, and it lets a user land between
    the named durations. Fill in the transport and pitch context, then resolve
    each side.

    No JUCE, no state, realtime safe. Header-only.

    Usage:

        fxme::DelayTimeResolver res;
        res.bpm = hostBpm;                    // from the playhead
        res.noteHz = lastNoteHz;              // 0 when nothing has played
        res.maxSeconds = maxDelayOfTheLine;
        const float seconds = res.toSeconds (value, mode);

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <algorithm>

namespace fxme
{

/** How a delay-time value is read. Stored as an index in host state, so the
    order only ever extends at the tail. */
enum class DelayTimeMode
{
    seconds = 0,
    tempoSync,
    notePeriod
};

struct DelayTimeResolver
{
    /** Host tempo. Only used by tempoSync; 120 is the usual fallback when the
        host offers nothing. */
    double bpm = 120.0;

    /** Pitch the notePeriod mode locks to, in Hz. Zero means nothing has
        played yet, and the value is then read as plain seconds so the delay
        still does something musical instead of falling silent. */
    float noteHz = 0.0f;

    /** The result is always clamped to this window, which should match what
        the delay line was actually prepared for: tempoSync and notePeriod can
        both ask for far more than the parameter's own range suggests. */
    float minSeconds = 0.0005f;
    float maxSeconds = 6.0f;

    /** Length of a whole note at the current tempo, in seconds. */
    double wholeNoteSeconds() const
    {
        const double safeBpm = bpm > 1.0 ? bpm : 120.0;
        return 4.0 * 60.0 / safeBpm;
    }

    float toSeconds (float value, DelayTimeMode mode) const
    {
        double seconds = (double) value;

        switch (mode)
        {
            case DelayTimeMode::tempoSync:
                seconds = (double) value * wholeNoteSeconds();
                break;

            case DelayTimeMode::notePeriod:
                if (noteHz > 0.0f)
                    seconds = (double) value / (double) noteHz;
                break;

            case DelayTimeMode::seconds:
            default:
                break;
        }

        return std::clamp ((float) seconds, minSeconds, maxSeconds);
    }
};

} // namespace fxme
