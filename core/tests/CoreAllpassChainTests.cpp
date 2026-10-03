/*
  ------------------------------------------------------------------------------
    CoreAllpassChainTests.cpp

    fxme::AllpassChain, measured on its impulse response:

      1. Every section passes every frequency at unit gain.
      2. Each section is -90 degrees at its break frequency (so the chain is
         -90 x stages there), and close to 0 well below it.
      3. Summed with the dry signal (a phaser at 100 % depth of notch), four
         stages put two deep notches either side of the break frequency, in
         the audible range, for break frequencies from 250 Hz to 2 kHz.

    Exit code 0 when everything passes.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/dsp/AllpassChain.h>

#include <cmath>
#include <complex>
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
    constexpr double fs = 48000.0;
    constexpr double pi = 3.141592653589793238;

    std::vector<double> impulseResponse (float fc, int stages, int length = 16384)
    {
        fxme::AllpassChain chain;
        chain.prepare (fs);
        chain.setNumStages (stages);
        chain.setFrequency (fc);

        std::vector<double> h ((size_t) length);
        for (int i = 0; i < length; ++i)
            h[(size_t) i] = chain.process (i == 0 ? 1.0f : 0.0f);
        return h;
    }

    std::complex<double> response (const std::vector<double>& h, double hz)
    {
        std::complex<double> sum = 0.0;
        const double w = 2.0 * pi * hz / fs;
        for (size_t n = 0; n < h.size(); ++n)
            sum += h[n] * std::polar (1.0, -w * (double) n);
        return sum;
    }

    double degrees (std::complex<double> z) { return std::arg (z) * 180.0 / pi; }
}

int main()
{
    std::printf ("AllpassChain\n");
    char what[160];

    // ---- 1 and 2: one section -------------------------------------------------
    for (float fc : { 250.0f, 1000.0f, 4000.0f })
    {
        const auto h = impulseResponse (fc, 1);

        bool unit = true;
        for (double hz : { 50.0, 500.0, 2000.0, 8000.0, 16000.0 })
            unit = unit && std::abs (std::abs (response (h, hz)) - 1.0) < 1.0e-3;
        std::snprintf (what, sizeof what, "fc %.0f Hz: unit gain everywhere", fc);
        check (unit, what);

        const double atFc = degrees (response (h, fc));
        std::snprintf (what, sizeof what, "fc %.0f Hz: -90 degrees at fc (%.1f)", fc, atFc);
        check (std::abs (atFc + 90.0) < 1.0, what);

        const double below = degrees (response (h, fc / 20.0));
        std::snprintf (what, sizeof what, "fc %.0f Hz: near 0 degrees far below fc (%.1f)", fc, below);
        check (std::abs (below) < 10.0, what);
    }

    // ---- 3: a phaser's notches ----------------------------------------------
    for (float fc : { 250.0f, 500.0f, 1000.0f, 2000.0f })
    {
        const auto h = impulseResponse (fc, 4);

        // Scan 20 Hz to 20 kHz for the dips of (dry + wet) / 2.
        std::vector<double> freqs, mags;
        for (int i = 0; i <= 800; ++i)
        {
            const double hz = 20.0 * std::pow (1000.0, i / 800.0);
            freqs.push_back (hz);
            mags.push_back (std::abs (1.0 + response (h, hz)) * 0.5);
        }

        int deepBelow = 0, deepAbove = 0;
        for (size_t i = 1; i + 1 < mags.size(); ++i)
            if (mags[i] < mags[i - 1] && mags[i] <= mags[i + 1] && mags[i] < 0.1)
                (freqs[i] < fc ? deepBelow : deepAbove)++;

        std::snprintf (what, sizeof what, "4 stages at fc %.0f Hz: a deep notch below fc and one above (%d / %d)",
                       fc, deepBelow, deepAbove);
        check (deepBelow == 1 && deepAbove == 1, what);
    }

    std::printf ("\n%s (%d failures)\n", failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
