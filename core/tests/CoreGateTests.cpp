/*
  ------------------------------------------------------------------------------
    CoreGateTests.cpp

    fxme::GateStateMachine:

      1. Closed under the open threshold, open at it.
      2. Hysteresis: once open, a level between the two thresholds keeps it
         open; under the close threshold it closes (straight away with no
         hold), and between the thresholds again it stays closed.
      3. Hold: closes exactly holdSamples samples after the level fell under
         the close threshold, and a level back over the close threshold
         during the hold reopens it (and restarts the count next time).
      4. A decaying, wobbling level (a note dying out with a +-2 dB ripple)
         makes one open -> closed transition with 6 dB of hysteresis, and
         chatters without it.

    Exit code 0 when everything passes.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/dsp/GateStateMachine.h>

#include <cmath>
#include <cstdio>

static int failures = 0;
static void check (bool ok, const char* what)
{
    std::printf ("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok)
        ++failures;
}

using State = fxme::GateStateMachine::State;

int main()
{
    constexpr double sampleRate = 1000.0;   // one sample per millisecond

    std::printf ("1. Thresholds\n");
    {
        fxme::GateStateMachine g;
        g.prepare (sampleRate);
        g.setThresholds (-40.0f, 0.0f);
        check (g.process (-41.0f) == State::closed, "closed under the threshold");
        check (g.process (-40.0f) == State::open, "open at the threshold");
    }

    std::printf ("2. Hysteresis\n");
    {
        fxme::GateStateMachine g;
        g.prepare (sampleRate);
        g.setThresholds (-40.0f, 6.0f);
        check (g.getCloseThresholdDb() == -46.0f, "close threshold 6 dB under");
        g.process (-30.0f);
        check (g.process (-45.0f) == State::open, "open between the thresholds");
        check (g.process (-47.0f) == State::closed, "closed under the close threshold (no hold)");
        check (g.process (-45.0f) == State::closed, "stays closed between the thresholds");
    }

    std::printf ("3. Hold\n");
    {
        fxme::GateStateMachine g;
        g.prepare (sampleRate);
        g.setThresholds (-40.0f, 0.0f);
        g.setHold (0.010f);   // 10 samples
        check (g.getHoldSamples() == 10, "10 ms is 10 samples at 1 kHz");

        g.process (-20.0f);
        int n = 0;
        while (g.process (-60.0f) != State::closed && n < 100)
            ++n;
        // The first quiet sample enters hold; the gate closes holdSamples
        // samples later, on the 11th quiet one. n counts those spent in hold.
        check (n == 10, "closes 10 samples after the level fell");

        g.process (-20.0f);
        for (int i = 0; i < 5; ++i)
            g.process (-60.0f);
        check (g.getState() == State::hold, "in hold after 5 quiet samples");
        check (g.process (-35.0f) == State::open, "reopened during the hold");
        n = 0;
        while (g.process (-60.0f) != State::closed && n < 100)
            ++n;
        check (n == 10, "a full hold again after reopening");
    }

    std::printf ("4. Decaying note with ripple\n");
    {
        auto transitions = [] (float hysteresis)
        {
            fxme::GateStateMachine g;
            g.prepare (sampleRate);
            g.setThresholds (-40.0f, hysteresis);
            int closings = 0;
            bool wasOpen = false;
            for (int i = 0; i < 2000; ++i)
            {
                // -20 dB falling to -60 over two seconds, +-2 dB at 50 Hz.
                const float level = -20.0f - 0.02f * (float) i + 2.0f * (float) std::sin (2.0 * 3.141592653589793 * 50.0 * i / 1000.0 + 0.3);
                const bool open = g.process (level) != State::closed;
                if (wasOpen && ! open)
                    ++closings;
                wasOpen = open;
            }
            return closings;
        };
        check (transitions (6.0f) == 1, "one closing with 6 dB of hysteresis");
        check (transitions (0.0f) > 1, "chatters without hysteresis or hold");
    }

    std::printf (failures == 0 ? "All gate tests passed\n" : "%d gate test(s) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
