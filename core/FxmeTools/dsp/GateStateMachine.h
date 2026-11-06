/*
  ------------------------------------------------------------------------------
    GateStateMachine.h

    The open / hold / closed logic of a noise gate, apart from its ballistics
    and its gain law, so that a gate, an expander with a hold, a ducker or a
    multiband gate can share it.

      closed   the level is under the open threshold. Goes to open when the
               level reaches it.
      open     the level has reached the open threshold and not fallen under
               the close threshold since. Goes to hold when it falls under the
               close threshold (straight to closed if the hold time is 0).
      hold     the level is under the close threshold, but for less than the
               hold time. Back to open if it comes back over the close
               threshold, to closed when the hold time runs out.

    The close threshold is the open one minus the hysteresis, so a decaying
    note that wobbles around the open threshold does not make the gate
    chatter: once open, it takes a level `hysteresisDb` lower to start
    closing. The hold, in turn, rides over the dips between the peaks of a
    low note or a fast tremolo.

    What the caller does with the state is its own business; the usual gate
    is "0 dB unless closed", smoothed with an attack (opening) and a release
    (closing). isOpen() is true in hold as well.

    The level is in dB, as fxme::LevelDetector gives it. Header-only, no
    allocation, realtime safe. Times in seconds, so the behaviour does not
    depend on the sample rate.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/util/Math.h>
#include <cmath>

namespace fxme
{

class GateStateMachine
{
public:
    enum class State { closed = 0, open, hold };

    void prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
        setHold (holdSeconds);
        reset();
    }

    void reset (State s = State::closed) noexcept
    {
        state = s;
        holdCounter = 0;
    }

    /** The open threshold, and how far under it the close threshold sits
        (0 or more). */
    void setThresholds (float openDb, float hysteresisDb) noexcept
    {
        openThresholdDb = openDb;
        closeThresholdDb = openDb - fxme::jmax (0.0f, hysteresisDb);
    }

    void setHold (float seconds) noexcept
    {
        holdSeconds = fxme::jmax (0.0f, seconds);
        holdSamples = (long) std::lround ((double) holdSeconds * sampleRate);
    }

    /** One level (dB) in, the state after it out. */
    State process (float levelDb) noexcept
    {
        switch (state)
        {
            case State::closed:
                if (levelDb >= openThresholdDb)
                    state = State::open;
                break;

            case State::open:
                if (levelDb < closeThresholdDb)
                    startHold();
                break;

            case State::hold:
                if (levelDb >= closeThresholdDb)
                    state = State::open;
                else if (--holdCounter <= 0)
                    state = State::closed;
                break;
        }
        return state;
    }

    State getState() const noexcept        { return state; }
    bool isOpen() const noexcept           { return state != State::closed; }
    float getOpenThresholdDb() const noexcept  { return openThresholdDb; }
    float getCloseThresholdDb() const noexcept { return closeThresholdDb; }
    long getHoldSamples() const noexcept   { return holdSamples; }

private:
    void startHold() noexcept
    {
        if (holdSamples <= 0)
        {
            state = State::closed;
            return;
        }
        state = State::hold;
        holdCounter = holdSamples;
    }

    double sampleRate = 48000.0;
    float openThresholdDb = -40.0f, closeThresholdDb = -44.0f;
    float holdSeconds = 0.0f;
    long holdSamples = 0, holdCounter = 0;
    State state = State::closed;
};

} // namespace fxme
