/*
  ------------------------------------------------------------------------------
    CoreLevelMatcherTests.cpp

    fxme::LevelMatcher behind fxme::Saturator, the case it was written for:

      1. The problem: dividing a saturator's output by its drive keeps small
         signals at unity, but a clipping one loses level as the drive rises.
      2. With the matcher after that division, the output's RMS equals the
         input's (within 0.1 dB) at every drive from 0 to 48 dB, for a loud
         and for a quiet tone, and for every saturator model. (Except where
         the saturator itself outputs nothing: Class AB's crossover notch
         swallows a quiet tone whole at low drive, and there is nothing to
         bring back.)
      3. Silence holds the gain: after a pause, a tone at the same level comes
         back at the matched level from its first samples.

    Exit code 0 when everything passes.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/dsp/LevelMatcher.h>
#include <FxmeTools/dsp/Saturator.h>

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

    double db (double x) { return 20.0 * std::log10 (std::max (x, 1.0e-12)); }

    /** RMS of `seconds` of a 220 Hz tone at `amplitude`, through the
        saturator at `driveDb`, divided by the drive, and matched or not.
        Measured over the last half second. */
    double outputRms (fxme::Saturator::Model model, float amplitude, float driveDb, bool match,
                      double seconds = 2.0)
    {
        fxme::Saturator sat;
        sat.prepare (sampleRate);
        sat.setModel (model);
        sat.setSag (model == fxme::Saturator::Model::Standard ? 0.0f : 0.5f);
        sat.setBias (model == fxme::Saturator::Model::ClassAB ? -0.1f : 0.0f);
        const float g = std::pow (10.0f, driveDb / 20.0f);
        sat.setDriveGain (g);

        fxme::LevelMatcher matcher;
        matcher.prepare (sampleRate);

        const int total = (int) (seconds * sampleRate);
        const int from  = total - (int) (0.5 * sampleRate);
        double sum = 0.0;
        for (int i = 0; i < total; ++i)
        {
            const float x = amplitude * (float) std::sin (2.0 * pi * 220.0 * i / sampleRate);
            float y = sat.processSample (x) / g;
            if (match)
                y = matcher.process (x, y);
            if (i >= from)
                sum += (double) y * y;
        }
        return std::sqrt (sum / (double) (total - from));
    }
}

int main()
{
    std::printf ("LevelMatcher\n");
    using Model = fxme::Saturator::Model;

    // ---- 1. the problem ----------------------------------------------------
    {
        const double at0  = outputRms (Model::Standard, 0.5f, 0.0f, false);
        const double at36 = outputRms (Model::Standard, 0.5f, 36.0f, false);
        char what[160];
        std::snprintf (what, sizeof what,
                       "dividing by the drive alone: a loud tone loses %.1f dB at 36 dB of drive",
                       db (at0) - db (at36));
        check (db (at0) - db (at36) > 20.0, what);
    }

    // ---- 2. matched at every drive ------------------------------------------
    for (Model model : { Model::Standard, Model::Dynamic, Model::Triode, Model::ClassAB })
        for (float amplitude : { 0.5f, 0.03f })
        {
            const double inRms = amplitude / std::sqrt (2.0);
            double worst = 0.0;
            for (float drive = 0.0f; drive <= 48.0f; drive += 6.0f)
                if (outputRms (model, amplitude, drive, false) > 0.0)
                    worst = std::max (worst, std::abs (db (outputRms (model, amplitude, drive, true)) - db (inRms)));

            char what[160];
            std::snprintf (what, sizeof what,
                           "model %d, tone at %.0f dBFS: matched within %.3f dB from 0 to 48 dB of drive",
                           (int) model, db (amplitude), worst);
            check (worst < 0.1, what);
        }

    // ---- 3. silence holds the gain -------------------------------------------
    {
        fxme::LevelMatcher matcher;
        matcher.prepare (sampleRate);
        const float processedScale = 0.1f;          // a processing that loses 20 dB

        const auto tone = [] (int i) { return 0.3f * (float) std::sin (2.0 * pi * 220.0 * i / sampleRate); };
        for (int i = 0; i < (int) sampleRate; ++i)
            matcher.process (tone (i), tone (i) * processedScale);
        const float settled = matcher.getGain();

        for (int i = 0; i < 2 * (int) sampleRate; ++i)
            matcher.process (0.0f, 0.0f);

        check (std::abs (matcher.getGain() - settled) < 1.0e-3f * settled,
               "two seconds of silence leave the gain where it was");
        check (std::abs (db (settled) - 20.0) < 0.1, "and that gain is the 20 dB the processing lost");
    }

    std::printf ("\n%s (%d failures)\n",
                 failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
