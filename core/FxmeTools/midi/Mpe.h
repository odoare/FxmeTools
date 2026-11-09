/*
  ------------------------------------------------------------------------------
    Mpe.h

    MIDI Polyphonic Expression, framework-free, both ways.

    MpeOut sends notes as MPE, so that a pitch reaches the instrument exact and
    each note can move on its own: each note on a channel of its own, 2 to 16
    (the lower zone; channel 1 is its master), its pitch the MIDI note plus a
    pitch bend sent just before the note-on, over +/-48 semitones (the MPE
    default, announced once by RPN 0 with the zone's configuration). Channels are
    handed out least recently used first, so a releasing note keeps its bend. With
    MPE off, every note goes out on one channel, its pitch rounded.

    MpeIn reads pitch bends as an MPE instrument hears them: a bend per channel,
    its range from RPN 0 (by default 48 semitones on channels 2..16, 2 on channel
    1), channel 1's bend added to every channel.

    The output is whatever the host writes MIDI to: any object with

        void noteOn (double time, int channel, int note, int velocity);
        void noteOff (double time, int channel, int note);
        void pitchBend (double time, int channel, int value);       // -8192..8191
        void controller (double time, int channel, int number, int value);

    Channels are 0-based here (0..15). `time` is in the host's unit (beats, samples):
    it is only handed on.

    Real time: no allocation, no lock.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace fxme
{

class MpeOut
{
public:
    static constexpr int bendRange = 48;                     ///< semitones, both ways
    static constexpr int firstMember = 1, lastMember = 15;   ///< channels 2..16, 0-based

    /** A note as it went out: what noteOff() needs. `note` -1: nothing was sent. */
    struct Note
    {
        int channel = 0;
        int note = -1;
        explicit operator bool() const noexcept { return note >= 0; }
        bool operator== (const Note& o) const noexcept { return channel == o.channel && note == o.note; }
        bool operator!= (const Note& o) const noexcept { return ! (*this == o); }
    };

    /** Switching MPE on announces the zone again before the next note. */
    void setMpe (bool mpeOn) noexcept
    {
        if (mpeOn && ! on)
            announced = false;
        on = mpeOn;
    }
    bool mpe() const noexcept { return on; }

    /** The channel when MPE is off, 0-based. */
    void setChannel (int channel) noexcept { base = channel < 0 ? 0 : channel > 15 ? 15 : channel; }
    int channel() const noexcept { return base; }

    /** Announces the zone again before the next note (a new instrument, a transport restart). */
    void reset() noexcept { announced = false; }

    /** `pitch` in MIDI notes, fractional for a microtonal pitch (60.5: a quarter tone above
        middle C). Nothing is sent out of 0..127 once rounded. */
    template <typename Out>
    Note noteOn (Out& out, double time, float pitch, int velocity) noexcept
    {
        const long rounded = std::lround (pitch);
        if (rounded < 0 || rounded > 127)
            return {};
        const int note = static_cast<int> (rounded);
        if (! on)
        {
            out.noteOn (time, base, note, velocity);
            return { base, note };
        }
        if (! announced)
            announce (out, time);
        const int c = pick();
        ++held[index (c)];
        used[index (c)] = ++clock;
        out.pitchBend (time, c, bendValue (pitch - static_cast<float> (note)));
        out.noteOn (time, c, note, velocity);
        return { c, note };
    }

    template <typename Out>
    void noteOff (Out& out, double time, Note n) noexcept
    {
        if (! n)
            return;
        out.noteOff (time, n.channel, n.note);
        if (n.channel >= firstMember && n.channel <= lastMember && held[index (n.channel)] > 0)
        {
            --held[index (n.channel)];
            used[index (n.channel)] = ++clock;   // released last: reused last
        }
    }

    /** Moves a sounding note to `pitch` (a glide's point, a legato's arrival) by its channel's
        bend. MPE only; false when MPE is off or the pitch is beyond bendRange of the note. */
    template <typename Out>
    bool bend (Out& out, double time, Note n, float pitch) noexcept
    {
        const float semitones = pitch - static_cast<float> (n.note);
        if (! on || ! n || n.channel < firstMember || std::fabs (semitones) > static_cast<float> (bendRange))
            return false;
        out.pitchBend (time, n.channel, bendValue (semitones));
        return true;
    }

    /** The 14-bit bend for `semitones` over +/-bendRange, -8192..8191. */
    static int bendValue (float semitones) noexcept
    {
        const long v = std::lround (static_cast<double> (semitones) / bendRange * 8192.0);
        return static_cast<int> (v < -8192 ? -8192 : v > 8191 ? 8191 : v);
    }

private:
    static constexpr std::size_t index (int channel) noexcept { return static_cast<std::size_t> (channel); }

    // The MPE configuration message (RPN 6 on the master channel: a lower zone of 15 channels),
    // then RPN 0 (the bend range) on each member channel; RPN null after each.
    template <typename Out>
    void announce (Out& out, double time) noexcept
    {
        rpn (out, time, 0, 6, lastMember);
        for (int c = firstMember; c <= lastMember; ++c)
            rpn (out, time, c, 0, bendRange);
        announced = true;
    }

    template <typename Out>
    static void rpn (Out& out, double time, int channel, int lsb, int value) noexcept
    {
        out.controller (time, channel, 101, 0);
        out.controller (time, channel, 100, lsb);
        out.controller (time, channel, 6, value);
        out.controller (time, channel, 38, 0);
        out.controller (time, channel, 101, 127);
        out.controller (time, channel, 100, 127);
    }

    /** A free channel, the least recently used; all busy: the least recently used anyway. */
    int pick() const noexcept
    {
        int best = -1, oldest = -1;
        for (int c = firstMember; c <= lastMember; ++c)
        {
            if (held[index (c)] == 0 && (best < 0 || used[index (c)] < used[index (best)]))
                best = c;
            if (oldest < 0 || used[index (c)] < used[index (oldest)])
                oldest = c;
        }
        return best >= 0 ? best : oldest;
    }

    bool on = true;
    bool announced = false;
    int base = 0;
    std::array<int, 16> held {};
    std::array<std::uint32_t, 16> used {};
    std::uint32_t clock = 0;
};

//==============================================================================
class MpeIn
{
public:
    MpeIn() noexcept { clear(); }

    /** Reads a pitch bend or the controllers of RPN 0, from a host's event with
        isPitchBend(), pitchBendValue(), isControlChange(), controller(), controlValue() and
        channel() (0-based). True if it was one of them. */
    template <typename Event>
    bool read (const Event& e) noexcept
    {
        if (e.isPitchBend())
        {
            readPitchBend (e.channel(), e.pitchBendValue());
            return true;
        }
        return e.isControlChange() && readController (e.channel(), e.controller(), e.controlValue());
    }

    /** `channel` 0-based, `value` -8192..8191. */
    void readPitchBend (int channel, int value) noexcept
    {
        const auto c = index (channel);
        bends[c] = static_cast<float> (value) / 8192.0f * ranges[c];
    }

    /** True if the controller belongs to RPN 0 (a bend range). */
    bool readController (int channel, int number, int value) noexcept
    {
        const auto c = index (channel);
        switch (number)
        {
            case 101: rpnMsb[c] = value; return true;
            case 100: rpnLsb[c] = value; return true;
            case 6:
                if (rpnMsb[c] != 0 || rpnLsb[c] != 0)
                    return false;   // another RPN (or an NRPN's data): not ours
                ranges[c] = static_cast<float> (value);
                return true;
            case 38:
                if (rpnMsb[c] != 0 || rpnLsb[c] != 0)
                    return false;
                ranges[c] = std::floor (ranges[c]) + static_cast<float> (value) / 100.0f;   // cents
                return true;
            default:
                return false;
        }
    }

    /** The bend a note on `channel` (0-based) plays with, in semitones: its own and the master's. */
    float bend (int channel) const noexcept
    {
        const auto c = index (channel);
        return bends[c] + (c != 0 ? bends[0] : 0.0f);
    }

    /** The bend of `channel` alone, without the master channel's. */
    float own (int channel) const noexcept { return bends[index (channel)]; }

    void clear() noexcept
    {
        bends.fill (0.0f);
        ranges.fill (48.0f);
        ranges[0] = 2.0f;
        rpnMsb.fill (127);
        rpnLsb.fill (127);
    }

private:
    static constexpr std::size_t index (int channel) noexcept { return static_cast<std::size_t> (channel & 0x0F); }

    std::array<float, 16> bends {};
    std::array<float, 16> ranges {};
    std::array<int, 16> rpnMsb {}, rpnLsb {};
};

} // namespace fxme
