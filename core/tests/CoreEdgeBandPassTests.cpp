/*
  ------------------------------------------------------------------------------
    CoreEdgeBandPassTests.cpp

    fxme::EdgeBandPass on steady tones:

      1. With both edges at the ends of the audible range, both stages drop
         out and the signal passes untouched, sample for sample.
      2. Inside a band, a tone passes at its level; two octaves outside, on
         either side, it is at least 40 dB down (24 dB per octave).
      3. A moved edge glides to its new place and the filter settles there.
      4. The honest limit: a band half an octave wide loses some level at its
         centre (the two slopes overlap), and the filter still behaves.

    Exit code 0 when everything passes.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/dsp/EdgeBandPass.h>

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
    constexpr int    blockSize  = 256;
    constexpr double pi         = 3.141592653589793238;

    /** Level change in dB of a steady tone through `filter`, measured over the
        last quarter of `seconds`, once it has settled. */
    double gainDb (fxme::EdgeBandPass& filter, double hz, double seconds = 0.6)
    {
        const int total = (int) (seconds * sampleRate);
        const int from  = total - total / 4;
        std::vector<float> block ((size_t) blockSize);
        double inSum = 0.0, outSum = 0.0;

        for (int start = 0; start < total; start += blockSize)
        {
            for (int i = 0; i < blockSize; ++i)
                block[(size_t) i] = 0.5f * (float) std::sin (2.0 * pi * hz * (start + i) / sampleRate);

            for (int i = 0; i < blockSize; ++i)
                if (start + i >= from)
                    inSum += (double) block[(size_t) i] * block[(size_t) i];

            filter.process (block.data(), blockSize);

            for (int i = 0; i < blockSize; ++i)
                if (start + i >= from)
                    outSum += (double) block[(size_t) i] * block[(size_t) i];
        }

        return 10.0 * std::log10 (outSum / inSum);
    }

    fxme::EdgeBandPass make (float low, float high)
    {
        fxme::EdgeBandPass f;
        f.prepare (sampleRate);
        f.setEdges (low, high, true);
        return f;
    }
}

int main()
{
    std::printf ("EdgeBandPass\n");

    // ---- 1. full range is a pass-through -------------------------------------
    {
        auto f = make (20.0f, 20000.0f);
        check (! f.isHighPassActive() && ! f.isLowPassActive(), "full range drops both stages");

        std::vector<float> x (1024), y;
        for (size_t i = 0; i < x.size(); ++i)
            x[i] = (float) std::sin (0.37 * (double) i) * 0.8f;
        y = x;
        f.process (y.data(), (int) y.size());
        check (x == y, "and passes the signal untouched, sample for sample");
    }

    // ---- 2. in band, out of band ---------------------------------------------
    {
        char what[160];
        auto f = make (200.0f, 2000.0f);

        const double inside = gainDb (f, 632.0);   // geometric centre
        std::snprintf (what, sizeof what, "632 Hz inside 200-2000 Hz passes within 1 dB (%.2f dB)", inside);
        check (std::abs (inside) < 1.0, what);

        f.reset();
        const double above = gainDb (f, 8000.0);
        std::snprintf (what, sizeof what, "8 kHz, two octaves above, is 40 dB down or more (%.1f dB)", above);
        check (above < -40.0, what);

        f.reset();
        const double below = gainDb (f, 50.0, 1.2);
        std::snprintf (what, sizeof what, "50 Hz, two octaves below, is 40 dB down or more (%.1f dB)", below);
        check (below < -40.0, what);
    }

    // ---- 3. a moved edge glides and settles ------------------------------------
    {
        auto f = make (200.0f, 20000.0f);
        f.setEdges (800.0f, 20000.0f);           // smoothed, not snapped
        const double settled = gainDb (f, 400.0);
        char what[160];
        std::snprintf (what, sizeof what,
                       "after moving the low edge to 800 Hz, 400 Hz is an octave below it (%.1f dB)", settled);
        check (settled < -18.0 && std::isfinite (settled), what);
    }

    // ---- 4. a narrow band -----------------------------------------------------
    {
        auto f = make (1000.0f, 1414.0f);        // half an octave
        const double centre = gainDb (f, 1189.0);
        char what[160];
        std::snprintf (what, sizeof what,
                       "a half-octave band loses some level at its centre, finitely (%.2f dB)", centre);
        check (centre < -0.5 && centre > -12.0, what);
    }

    std::printf ("\n%s (%d failures)\n",
                 failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
