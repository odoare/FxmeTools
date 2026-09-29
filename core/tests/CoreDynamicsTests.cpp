/*
  ------------------------------------------------------------------------------
    CoreDynamicsTests.cpp

    fxme::BandDynamics / fxme::DynamicsCurve, and the two splitters using it:

      1. The static curve: an expander of ratio r takes a level d dB under the
         line to (r - 1) d dB of gain, never under its range; a hard gate is
         closed (to its range) under the line; a compressor of ratio r takes
         d dB over the line to -(1 - 1/r) d dB; a limiter to -d; a cut to 0.
         Across a knee the curve is continuous and monotonic.
      2. Live (filter bank): a steady tone 12 dB over a 4:1 ceiling comes out
         9 dB down; 10 dB under a 1:2 gate, 10 dB down.
      2b. The compressor's times go the right way: its attack is how fast it
         pulls the gain down when the level rises over the line, its release
         how fast it lets it back up (the gate's are the other way round:
         opening is its attack).
      3. Spectral: the same compressor acts bin by bin. A bin-centred tone
         spreads over three bins through the Hann window (the centre, and its
         neighbours 6 dB lower), each compressed by its own level: -9 dB on
         the centre, -4.5 on the neighbours. Through the synthesis window and
         the overlap-add, the tone's amplitude comes back as 2/3 of the
         centre's gain plus 1/3 of the neighbours' (with Hann = 1/2 - 1/2 cos,
         four frames at 75 % overlap sum w (a - b cos) to 2a + b, over the
         1.5 of w^2), so about -7.2 dB overall.

    Exit code 0 when everything passes.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/dsp/BandDynamics.h>
#include <FxmeTools/dsp/FilterBankSplitter.h>
#include <FxmeTools/dsp/SpectralBandSplitter.h>

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
    const float inf = fxme::BandDynamics::infinity;

    double db (double x) { return 20.0 * std::log10 (std::max (x, 1.0e-12)); }

    /** Squared level `dbFromLine` dB away from a line at level 1. */
    float at (float dbFromLine) { return std::pow (10.0f, dbFromLine / 10.0f); }

    fxme::DynamicsCurve curve (fxme::BandDynamics d, bool gate, bool ceiling)
    {
        fxme::DynamicsCurve c;
        c.set (gate ? 1.0f : 0.0f, ceiling ? 1.0f : inf, d);
        return c;
    }

    bool near (double a, double b, double tol) { return std::abs (a - b) < tol; }
}

int main()
{
    std::printf ("Band dynamics\n");
    char what[200];

    // ---- 1. static curve -------------------------------------------------------------
    {
        fxme::BandDynamics d;
        d.gateRatio = 2.0f;
        auto c = curve (d, true, false);
        check (near (db (c.gateGain (at (-10.0f))), -10.0, 0.01), "expander 1:2, 10 dB under: -10 dB");
        check (c.gateGain (at (3.0f)) == 1.0f, "expander, over the line: untouched");

        d.gateRatio = 4.0f;
        c = curve (d, true, false);
        check (near (db (c.gateGain (at (-10.0f))), -30.0, 0.01), "expander 1:4, 10 dB under: -30 dB");

        d.gateRangeDb = -20.0f;
        c = curve (d, true, false);
        check (near (db (c.gateGain (at (-10.0f))), -20.0, 0.01), "expander 1:4 with a -20 dB range: stops at -20 dB");

        fxme::BandDynamics hard;
        c = curve (hard, true, false);
        check (c.gateGain (at (-1.0f)) == 0.0f && c.gateGain (at (0.0f)) == 1.0f,
               "hard gate: closed under the line, open on it");
        hard.gateRangeDb = -30.0f;
        c = curve (hard, true, false);
        check (near (db (c.gateGain (at (-10.0f))), -30.0, 0.01), "hard gate with a -30 dB range: -30 dB under the line");

        fxme::BandDynamics comp;
        comp.ceilingCut = false;
        comp.ceilingRatio = 4.0f;
        c = curve (comp, false, true);
        check (near (db (c.ceilingGain (at (12.0f))), -9.0, 0.01), "compressor 4:1, 12 dB over: -9 dB");
        check (c.ceilingGain (at (-1.0f)) == 1.0f, "compressor, under the line: untouched");
        comp.ceilingRatio = inf;
        c = curve (comp, false, true);
        check (near (db (c.ceilingGain (at (12.0f))), -12.0, 0.01), "limiter, 12 dB over: -12 dB");
        comp.ceilingRatio = 1.0f;
        c = curve (comp, false, true);
        check (! c.isCeilingOn(), "compressor 1:1: off");

        fxme::BandDynamics cut;
        c = curve (cut, false, true);
        check (c.ceilingGain (at (0.5f)) == 0.0f && c.ceilingGain (at (-0.5f)) == 1.0f,
               "cut: muted over the line, untouched under it");

        // Knees: continuous at their edges, monotonic across them.
        fxme::BandDynamics knee;
        knee.gateRatio = 3.0f;
        knee.gateKneeDb = 12.0f;
        knee.ceilingCut = false;
        knee.ceilingRatio = 5.0f;
        knee.ceilingKneeDb = 12.0f;
        c = curve (knee, true, false);
        auto cc = curve (knee, false, true);
        double worstJump = 0.0;
        bool monotonic = true;
        float lastGate = 0.0f, lastCeiling = 2.0f;
        for (float x = -20.0f; x <= 20.0f; x += 0.01f)
        {
            const float g = c.gateGain (at (x)), k = cc.ceilingGain (at (x));
            monotonic = monotonic && g >= lastGate - 1.0e-6f && k <= lastCeiling + 1.0e-6f;
            if (x > -20.0f)
                worstJump = std::max ({ worstJump, std::abs (db (g) - db (lastGate)),
                                                   std::abs (db (k) - db (lastCeiling)) });
            lastGate = g;
            lastCeiling = k;
        }
        std::snprintf (what, sizeof what,
                       "12 dB knees: continuous (largest step %.3f dB per 0.01 dB) and monotonic", worstJump);
        check (worstJump < 0.05 && monotonic, what);
    }

    // ---- 2. live: a classic compressor and expander ---------------------------------------
    {
        const float toneDb = 20.0f * std::log10 (0.25f);    // a 0.5 sine reads 0.25
        const auto run = [&] (float gateDb, float ceilingDb, const fxme::BandDynamics& d)
        {
            fxme::FilterBankSplitter bank;
            bank.prepare (sampleRate, 512, 1);
            bank.setApplyPan (false);
            bank.setBand (0, { true, 20.0f, 20000.0f, gateDb, 0.0f, 0.0f, ceilingDb });
            bank.setBandDynamics (0, d);
            bank.reset();

            double sum = 0.0;
            int count = 0;
            for (int start = 0; start < (int) sampleRate; start += 512)
            {
                std::vector<float> x (512);
                for (int i = 0; i < 512; ++i)
                    x[(size_t) i] = 0.5f * (float) std::sin (2.0 * pi * 1000.0 * (start + i) / sampleRate);
                bank.process (x.data(), 512);
                if (start >= (int) sampleRate / 2)
                    for (int i = 0; i < 512; ++i, ++count)
                        sum += (double) bank.getBandOutput (0, 0)[i] * bank.getBandOutput (0, 0)[i];
            }
            return db (std::sqrt (sum / count)) - db (0.5 / std::sqrt (2.0));
        };

        fxme::BandDynamics comp;
        comp.ceilingCut = false;
        comp.ceilingRatio = 4.0f;
        const double c = run (-1000.0f, toneDb - 12.0f, comp);
        std::snprintf (what, sizeof what, "live compressor 4:1, tone 12 dB over: %.2f dB (-9)", c);
        check (near (c, -9.0, 0.3), what);

        fxme::BandDynamics exp;
        exp.gateRatio = 2.0f;
        const double e = run (toneDb + 10.0f, 1000.0f, exp);
        std::snprintf (what, sizeof what, "live expander 1:2, tone 10 dB under: %.2f dB (-10)", e);
        check (near (e, -10.0, 0.3), what);
    }

    // ---- 2b. compressor attack and release --------------------------------------------------
    {
        fxme::BandDynamicsProcessor gate;
        gate.prepare (sampleRate);
        gate.setDetectorSeconds (0.0002f);   // short enough not to matter
        fxme::BandDynamics comp;
        comp.ceilingCut = false;
        comp.ceilingRatio = 4.0f;
        comp.ceilingAttackSeconds  = 0.002f;
        comp.ceilingReleaseSeconds = 0.5f;
        gate.setDynamics (comp);

        // DC at 0.5 reads 20 log10 (0.5 / sqrt 2): put the line 12 dB under.
        const float levelDb = 20.0f * std::log10 (0.5f / std::sqrt (2.0f));
        gate.setThresholds (fxme::BandDynamicsProcessor::openGateDb, levelDb - 12.0f);
        gate.reset();

        const float target = std::pow (10.0f, -9.0f / 20.0f);
        const float attackMark  = 1.0f - 0.632f * (1.0f - target);   // 63 % of the way down
        const float releaseMark = target + 0.632f * (1.0f - target); // 63 % of the way back up

        const auto timeTo = [&gate] (float input, float mark, bool falling)
        {
            std::vector<float> x (1);
            for (int i = 0; i < 2 * (int) sampleRate; ++i)
            {
                x[0] = input;
                gate.process (x.data(), 1);
                if (falling ? gate.getGain() <= mark : gate.getGain() >= mark)
                    return 1000.0 * i / sampleRate;
            }
            return -1.0;
        };

        const double attack = timeTo (0.5f, attackMark, true);
        timeTo (0.5f, -1.0f, true);                     // settle at the compressed level
        const double release = timeTo (0.0f, releaseMark, false);

        std::snprintf (what, sizeof what,
                       "compressor, attack 2 ms / release 500 ms: pulls down in %.1f ms, lets go in %.0f ms",
                       attack, release);
        check (attack > 0.0 && attack < 5.0 && release > 400.0 && release < 600.0, what);
    }

    // ---- 3. spectral: per bin ---------------------------------------------------------------
    {
        constexpr int order = 11, n = 1 << order, bin = 64;
        const float centreDb = 20.0f * std::log10 (0.5f * 0.5f);   // a 0.5 sine on a bin reads 0.25

        fxme::SpectralBandSplitter splitter;
        splitter.prepare (sampleRate, 512, 1, order);
        splitter.setApplyPan (false);
        splitter.setBand (0, { true, 20.0f, 20000.0f, -1000.0f, 0.0f, 0.0f, centreDb - 12.0f });
        fxme::BandDynamics comp;
        comp.ceilingCut = false;
        comp.ceilingRatio = 4.0f;
        splitter.setBandDynamics (0, comp);
        splitter.reset();

        double sum = 0.0;
        int count = 0;
        for (int start = 0; start < (int) sampleRate; start += 512)
        {
            std::vector<float> x (512);
            for (int i = 0; i < 512; ++i)
                x[(size_t) i] = 0.5f * (float) std::sin (2.0 * pi * bin * (start + i) / n);
            splitter.process (x.data(), 512);
            if (start >= (int) sampleRate / 2)
                for (int i = 0; i < 512; ++i, ++count)
                    sum += (double) splitter.getBandOutput (0, 0)[i] * splitter.getBandOutput (0, 0)[i];
        }
        const double out = db (std::sqrt (sum / count)) - db (0.5 / std::sqrt (2.0));

        // Centre at 12 dB over (-9 dB), neighbours at 6 dB over (-4.5 dB),
        // weighted 2/3 and 1/3 by the resynthesis (see the file comment).
        const double expected = db (2.0 / 3.0 * std::pow (10.0, -9.0 / 20.0)
                                    + 1.0 / 3.0 * std::pow (10.0, -4.5 / 20.0));
        std::snprintf (what, sizeof what,
                       "spectral compressor 4:1, per bin: %.2f dB (expected %.2f from the three bins)", out, expected);
        check (near (out, expected, 0.3), what);
    }

    std::printf ("\n%s (%d failures)\n",
                 failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
