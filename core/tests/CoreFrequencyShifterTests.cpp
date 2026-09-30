/*
  ------------------------------------------------------------------------------
    CoreFrequencyShifterTests.cpp

    fxme::FrequencyShifter:

      1. A tone lands where it should: up for a positive shift, down for a
         negative one, at its own level.
      2. Its mirror (the other sideband) is rejected by at least 40 dB from
         50 Hz to 15 kHz.
      3. Stereo: identical channels come out identical (one oscillator).

    Exit code 0 when everything passes.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/dsp/FrequencyShifter.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

static int failures = 0;
static void check (bool ok, const char* what)
{
    std::printf ("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok)
        ++failures;
}

namespace
{
    constexpr double sampleRate = 48000.0;
    constexpr double pi         = 3.141592653589793238;

    /** Power in dB at `f`, by Goertzel, from sample `from` on. */
    double powerAt (const std::vector<float>& x, double f, int from)
    {
        const double c = 2.0 * std::cos (2.0 * pi * f / sampleRate);
        double s1 = 0.0, s2 = 0.0;
        for (size_t i = (size_t) from; i < x.size(); ++i)
        {
            const double s0 = x[i] + c * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        return 10.0 * std::log10 (s1 * s1 + s2 * s2 - c * s1 * s2 + 1.0e-30);
    }

    std::vector<float> shiftTone (double toneHz, float shiftHz)
    {
        fxme::FrequencyShifter shifter;
        shifter.prepare (sampleRate, 1);
        shifter.setShiftHz (shiftHz);
        shifter.reset();

        std::vector<float> out ((size_t) sampleRate);
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = shifter.processSample (0.5f * (float) std::sin (2.0 * pi * toneHz * (double) i / sampleRate));
        return out;
    }

    std::vector<float> tone (double toneHz)
    {
        std::vector<float> x ((size_t) sampleRate);
        for (size_t i = 0; i < x.size(); ++i)
            x[i] = 0.5f * (float) std::sin (2.0 * pi * toneHz * (double) i / sampleRate);
        return x;
    }
}

int main()
{
    std::printf ("FrequencyShifter\n");
    char what[200];
    const int from = (int) sampleRate / 4;

    // ---- 1. where a tone lands ----------------------------------------------------------
    for (float shift : { 100.0f, -250.0f, 1000.0f })
    {
        const auto out = shiftTone (1000.0, shift);
        const double target = 1000.0 + shift;
        const double level = powerAt (out, target, from) - powerAt (tone (target), target, from);
        const double left  = powerAt (out, 1000.0, from) - powerAt (out, target, from);
        std::snprintf (what, sizeof what,
                       "1 kHz shifted %+.0f Hz lands at %.0f Hz at %+.2f dB, the original %.0f dB below",
                       shift, target, level, -left);
        check (std::abs (level) < 0.2 && left < -40.0, what);
    }

    // ---- 2. the mirror is rejected across the band -----------------------------------------
    {
        double worst = 1000.0;
        for (double f : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 15000.0 })
        {
            const auto out = shiftTone (f, 30.0f);
            worst = std::min (worst, powerAt (out, f + 30.0, from) - powerAt (out, f - 30.0, from));
        }
        std::snprintf (what, sizeof what, "the mirror sideband is at least %.1f dB down, 50 Hz to 15 kHz", worst);
        check (worst > 40.0, what);
    }

    // ---- 3. stereo -------------------------------------------------------------------------------
    {
        fxme::FrequencyShifter shifter;
        shifter.prepare (sampleRate, 2);
        shifter.setShiftHz (-70.0f);
        shifter.reset();

        float worst = 0.0f;
        for (int i = 0; i < (int) sampleRate / 2; ++i)
        {
            const float x = 0.4f * (float) std::sin (2.0 * pi * 440.0 * i / sampleRate);
            float frame[2] { x, x };
            shifter.processFrame (frame);
            worst = std::max (worst, std::abs (frame[0] - frame[1]));
        }
        check (worst == 0.0f, "identical channels come out identical");
    }

    std::printf ("\n%s (%d failures)\n",
                 failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
