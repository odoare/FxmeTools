/*
  ------------------------------------------------------------------------------
    synth/VoiceAllocator.h

    Which voice plays which note, for a polyphonic synth that also has mono
    and legato modes. It only decides; the synth applies its decisions
    (Actions) to its own voices and tells it when a voice falls silent.

      poly    a free voice if there is one among the first `polyphony`;
              otherwise steal the oldest *released* voice, and only then the
              oldest held one (the synth fades a stolen voice out over a few
              milliseconds before restarting it, so stealing does not click).
      mono    one voice; a new note retriggers its envelopes.
      legato  one voice; a note played while another is held only moves the
              pitch (envelopes keep running); from silence it starts as usual.

    Both single-voice modes keep a stack of held notes with last-note
    priority: releasing the sounding note returns to the previous held one
    (retriggering in mono, gliding in legato); releasing the last one
    releases the voice.

    Every action says whether another note was held when it started
    (`legato`), which is what a "glide in legato only" mode needs.

    The sustain pedal holds released notes until it comes up.

    Header-only, fixed size, no allocation, realtime safe.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <array>
#include <cstdint>

namespace fxme
{

class VoiceAllocator
{
public:
    static constexpr int maxVoices = 64;
    static constexpr int maxActions = maxVoices + 2;

    enum class Mode { poly = 0, mono, legato };

    struct Action
    {
        enum class Type
        {
            start,       // idle voice: start `note`
            steal,       // sounding voice: fade out quickly, then start `note`
            retrigger,   // single-voice mode: new pitch, envelopes restart from where they are
            glide,       // legato: new pitch only
            release      // note-off for this voice
        };

        Type type = Type::start;
        int voice = 0;
        int note = 60;
        float velocity = 0.0f;
        bool legato = false;     // another note was held when this one started
    };

    void setMode (Mode m) noexcept
    {
        if (m != mode)
        {
            mode = m;
            stackSize = 0;
        }
    }

    Mode getMode() const noexcept { return mode; }

    /** Number of voices to use in poly mode (1 .. the synth's voice count). */
    void setPolyphony (int n) noexcept { polyphony = n < 1 ? 1 : (n > maxVoices ? maxVoices : n); }
    int getPolyphony() const noexcept  { return polyphony; }

    /** Forgets everything (all voices idle). */
    void reset() noexcept
    {
        for (auto& v : voices)
            v = {};
        stackSize = 0;
        sustainDown = false;
        clock = 0;
    }

    //==========================================================================
    /** Handles a note-on; writes the actions to `out` (room for maxActions)
        and returns how many. */
    int noteOn (int note, float velocity, Action* out) noexcept
    {
        const bool otherHeld = anyGated();
        ++clock;

        if (mode == Mode::poly)
        {
            int v = findIdle();
            Action::Type type = Action::Type::start;
            if (v < 0)
            {
                v = findStealCandidate();
                type = Action::Type::steal;
            }
            assign (v, note);
            out[0] = { type, v, note, velocity, otherHeld };
            return 1;
        }

        // Single-voice modes.
        pushNote (note, velocity);
        auto& s = voices[0];
        Action::Type type = Action::Type::start;
        if (s.sounding)
            type = (mode == Mode::legato && otherHeld) ? Action::Type::glide : Action::Type::retrigger;
        assign (0, note);
        out[0] = { type, 0, note, velocity, otherHeld };
        return 1;
    }

    /** Handles a note-off. */
    int noteOff (int note, float releaseVelocity, Action* out) noexcept
    {
        int n = 0;

        if (mode == Mode::poly)
        {
            for (int v = 0; v < maxVoices; ++v)
            {
                auto& s = voices[(std::size_t) v];
                if (s.sounding && s.gated && s.note == note)
                {
                    if (sustainDown)
                        s.sustained = true;
                    else
                    {
                        s.gated = false;
                        out[n++] = { Action::Type::release, v, note, releaseVelocity, false };
                    }
                }
            }
            return n;
        }

        const bool wasTop = stackSize > 0 && stack[(std::size_t) stackSize - 1].note == note;
        removeNote (note);
        auto& s = voices[0];
        if (! s.sounding || ! s.gated || ! wasTop)
            return 0;

        if (stackSize > 0)
        {
            const auto& prev = stack[(std::size_t) stackSize - 1];
            s.note = prev.note;
            out[n++] = { mode == Mode::legato ? Action::Type::glide : Action::Type::retrigger,
                         0, prev.note, prev.velocity, true };
        }
        else if (sustainDown)
            s.sustained = true;
        else
        {
            s.gated = false;
            out[n++] = { Action::Type::release, 0, note, releaseVelocity, false };
        }
        return n;
    }

    /** Sustain pedal; releasing it releases every note it was holding. */
    int sustainPedal (bool down, Action* out) noexcept
    {
        sustainDown = down;
        if (down)
            return 0;

        int n = 0;
        for (int v = 0; v < maxVoices; ++v)
        {
            auto& s = voices[(std::size_t) v];
            if (s.sounding && s.gated && s.sustained)
            {
                s.gated = false;
                s.sustained = false;
                out[n++] = { Action::Type::release, v, s.note, 0.0f, false };
            }
        }
        return n;
    }

    /** Releases every held voice (all notes off). */
    int allNotesOff (Action* out) noexcept
    {
        int n = 0;
        stackSize = 0;
        for (int v = 0; v < maxVoices; ++v)
        {
            auto& s = voices[(std::size_t) v];
            if (s.sounding && s.gated)
            {
                s.gated = false;
                s.sustained = false;
                out[n++] = { Action::Type::release, v, s.note, 0.0f, false };
            }
        }
        return n;
    }

    /** The synth reports that voice `v` has fallen silent. */
    void voiceStopped (int v) noexcept
    {
        if (v >= 0 && v < maxVoices)
            voices[(std::size_t) v] = {};
    }

    bool isGated (int v) const noexcept { return voices[(std::size_t) v].gated; }

    /** The most recently started voice that is still sounding, or -1. */
    int newestVoice() const noexcept
    {
        int best = -1;
        std::uint64_t bestAge = 0;
        for (int v = 0; v < maxVoices; ++v)
        {
            const auto& s = voices[(std::size_t) v];
            if (s.sounding && (best < 0 || s.startedAt > bestAge))
            {
                best = v;
                bestAge = s.startedAt;
            }
        }
        return best;
    }

private:
    struct VoiceState
    {
        bool sounding = false, gated = false, sustained = false;
        int note = -1;
        std::uint64_t startedAt = 0;
    };

    struct HeldNote
    {
        int note = 0;
        float velocity = 0.0f;
    };

    bool anyGated() const noexcept
    {
        for (const auto& v : voices)
            if (v.sounding && v.gated)
                return true;
        return false;
    }

    int findIdle() const noexcept
    {
        for (int v = 0; v < polyphony; ++v)
            if (! voices[(std::size_t) v].sounding)
                return v;
        return -1;
    }

    int findStealCandidate() const noexcept
    {
        int best = -1;
        std::uint64_t bestAge = 0;
        // Released voices first, the oldest of them; then the oldest held.
        for (int pass = 0; pass < 2 && best < 0; ++pass)
            for (int v = 0; v < polyphony; ++v)
            {
                const auto& s = voices[(std::size_t) v];
                const bool candidate = pass == 0 ? ! s.gated : true;
                if (candidate && (best < 0 || s.startedAt < bestAge))
                {
                    best = v;
                    bestAge = s.startedAt;
                }
            }
        return best < 0 ? 0 : best;
    }

    void assign (int v, int note) noexcept
    {
        auto& s = voices[(std::size_t) v];
        s.sounding = true;
        s.gated = true;
        s.sustained = false;
        s.note = note;
        s.startedAt = clock;
    }

    void pushNote (int note, float velocity) noexcept
    {
        removeNote (note);
        if (stackSize == (int) stack.size())
        {
            for (int i = 1; i < stackSize; ++i)
                stack[(std::size_t) i - 1] = stack[(std::size_t) i];
            --stackSize;
        }
        stack[(std::size_t) stackSize++] = { note, velocity };
    }

    void removeNote (int note) noexcept
    {
        int w = 0;
        for (int r = 0; r < stackSize; ++r)
            if (stack[(std::size_t) r].note != note)
                stack[(std::size_t) w++] = stack[(std::size_t) r];
        stackSize = w;
    }

    Mode mode = Mode::poly;
    int polyphony = 8;
    bool sustainDown = false;
    std::uint64_t clock = 0;
    std::array<VoiceState, maxVoices> voices {};
    std::array<HeldNote, 32> stack {};
    int stackSize = 0;
};

} // namespace fxme
