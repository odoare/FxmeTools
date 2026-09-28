/*
  ------------------------------------------------------------------------------
    CoreSplitterTests.cpp

    The level gating of fxme::SpectralBandSplitter, measured on a steady tone:

      1. The test setup itself: an open band passes the tone at the expected
         level.
      2. The ceiling is off by default, and an explicit "off" value gives the
         same output sample for sample.
      3. The gate passes a tone above its line and removes one below it.
      4. The ceiling mirrors it: passes below, removes above.
      5. A ceiling under the gate leaves nothing to pass.
      6. A knee turns an outright switch into a partial gain, for both lines,
         and going back to a zero knee restores the hard switch exactly.
      7. With the pan turned off, both channels carry the same band at unity
         (gain only), whatever the band's pan; with it on, the pan applies.
      8. The gate openness a meter reads: 1 with no level gating, near 1 when
         the tone passes, near 0 when the gate removes it, 0 when disabled.
      9. fxme::SpectrumAnalyzer (what a spectrum view draws), in peak mode
         with no averaging and at the same window size, reads the tone at
         exactly the level the gate compares against, at every size the
         splitter and the view share.

    The tone sits exactly on bin 64 of the 2048-point window (1500 Hz at 48
    kHz), so through the Hann window it occupies three bins only: the centre
    one at level amplitude/2 (in the analyser convention, level = mag*2/N)
    and its two neighbours 6 dB lower. The margins below are chosen well
    clear of those three levels, so the checks do not depend on rounding.

    Exit code 0 when everything passes.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/dsp/SpectralBandSplitter.h>
#include <FxmeTools/dsp/SpectrumAnalyzer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <utility>
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
    constexpr int    order      = 11;
    constexpr int    fftSize    = 1 << order;
    constexpr int    blockSize  = 512;
    constexpr int    toneBin    = 64;
    constexpr float  amplitude  = 0.5f;

    /** Level of the tone's centre bin, in the splitter's dB convention. */
    const float toneDb = 20.0f * std::log10 (amplitude * 0.5f);

    fxme::SpectralBand fullBand()
    {
        fxme::SpectralBand b;
        b.enabled = true;
        b.lowHz   = 20.0f;
        b.highHz  = 20000.0f;
        return b;
    }

    /** Runs one second of the tone through a one-band splitter and returns the
        left output over the last quarter second, well past the latency and
        the gate's attack and release.

        The knee is set after the band, so the thresholds are recomputed on a
        band that already exists; `earlierKneeDb`, when not negative, is set
        before the band. A negative `kneeDb` leaves the knee untouched. */
    std::vector<float> run (const fxme::SpectralBand& band, float kneeDb,
                            float earlierKneeDb = -1.0f)
    {
        fxme::SpectralBandSplitter splitter;
        splitter.prepare (sampleRate, blockSize, 1, order);
        if (earlierKneeDb >= 0.0f)
            splitter.setGateKnee (earlierKneeDb);
        splitter.setBand (0, band);
        if (kneeDb >= 0.0f)
            splitter.setGateKnee (kneeDb);

        const int total = (int) sampleRate;
        const int keepFrom = total - total / 4;
        const double w = 2.0 * 3.141592653589793238 * toneBin / (double) fftSize;

        std::vector<float> in ((size_t) blockSize), kept;
        kept.reserve ((size_t) (total - keepFrom));

        for (int start = 0; start < total; start += blockSize)
        {
            for (int i = 0; i < blockSize; ++i)
                in[(size_t) i] = amplitude * (float) std::sin (w * (double) (start + i));

            splitter.process (in.data(), blockSize);

            const float* left = splitter.getBandOutput (0, 0);
            for (int i = 0; i < blockSize; ++i)
                if (start + i >= keepFrom)
                    kept.push_back (left[i]);
        }

        return kept;
    }

    /** Like run(), with the pan switched on or off, returning both channels
        over the same last quarter second. */
    std::pair<std::vector<float>, std::vector<float>>
    runStereo (const fxme::SpectralBand& band, bool applyPan)
    {
        fxme::SpectralBandSplitter splitter;
        splitter.prepare (sampleRate, blockSize, 1, order);
        splitter.setApplyPan (applyPan);
        splitter.setBand (0, band);

        const int total = (int) sampleRate;
        const int keepFrom = total - total / 4;
        const double w = 2.0 * 3.141592653589793238 * toneBin / (double) fftSize;

        std::vector<float> in ((size_t) blockSize), left, right;

        for (int start = 0; start < total; start += blockSize)
        {
            for (int i = 0; i < blockSize; ++i)
                in[(size_t) i] = amplitude * (float) std::sin (w * (double) (start + i));

            splitter.process (in.data(), blockSize);

            for (int i = 0; i < blockSize; ++i)
                if (start + i >= keepFrom)
                {
                    left.push_back (splitter.getBandOutput (0, 0)[i]);
                    right.push_back (splitter.getBandOutput (0, 1)[i]);
                }
        }

        return { left, right };
    }

    /** Runs the tone through a one-band splitter and returns the band's gate
        openness at the end. */
    float opennessAfter (const fxme::SpectralBand& band)
    {
        fxme::SpectralBandSplitter splitter;
        splitter.prepare (sampleRate, blockSize, 1, order);
        splitter.setBand (0, band);

        const double w = 2.0 * 3.141592653589793238 * toneBin / (double) fftSize;
        std::vector<float> in ((size_t) blockSize);
        for (int start = 0; start < (int) sampleRate; start += blockSize)
        {
            for (int i = 0; i < blockSize; ++i)
                in[(size_t) i] = amplitude * (float) std::sin (w * (double) (start + i));
            splitter.process (in.data(), blockSize);
        }
        return splitter.getGateOpenness (0);
    }

    double rms (const std::vector<float>& x)
    {
        double sum = 0.0;
        for (float v : x)
            sum += (double) v * v;
        return x.empty() ? 0.0 : std::sqrt (sum / (double) x.size());
    }
}

int main()
{
    std::printf ("SpectralBandSplitter level gating\n");

    // ---- 1. setup ----------------------------------------------------------
    const auto open = run (fullBand(), 0.0f);
    const double ref = rms (open);

    // A centred pan takes cos (pi/4) off each side of a sine of RMS a/sqrt 2.
    const double expected = amplitude / std::sqrt (2.0) * std::cos (3.141592653589793238 / 4.0);
    check (std::abs (ref - expected) < 0.05 * expected,
           "an open band passes the tone at the expected level");

    const auto passes  = [ref] (double r) { return std::abs (r - ref) < 0.05 * ref; };
    const auto removed = [ref] (double r) { return r < 0.01 * ref; };

    // ---- 2. ceiling off by default -----------------------------------------
    {
        auto b = fullBand();
        b.ceilingDb = 200.0f;
        check (run (b, 0.0f) == open, "an explicit 'off' ceiling changes nothing, sample for sample");

        check (fxme::SpectralBand{}.ceilingDb >= fxme::SpectralBandSplitter::offCeilingDb,
               "the default ceiling is off");

        // Brace-initialisation written before the ceiling existed still means
        // what it did: the ceiling is last, and stays off.
        const fxme::SpectralBand legacy { true, 200.0f, 2000.0f, -60.0f, -3.0f, -0.5f };
        check (legacy.gainDb == -3.0f && legacy.pan == -0.5f
                   && legacy.ceilingDb >= fxme::SpectralBandSplitter::offCeilingDb,
               "positional initialisation keeps its meaning");
    }

    // ---- 3. gate -----------------------------------------------------------
    {
        auto b = fullBand();
        b.gateDb = toneDb - 20.0f;
        check (passes (rms (run (b, 0.0f))), "a gate 20 dB under the tone passes it");

        b.gateDb = toneDb + 10.0f;
        check (removed (rms (run (b, 0.0f))), "a gate 10 dB over the tone removes it");
    }

    // ---- 4. ceiling --------------------------------------------------------
    {
        auto b = fullBand();
        b.ceilingDb = toneDb + 20.0f;
        check (passes (rms (run (b, 0.0f))), "a ceiling 20 dB over the tone passes it");

        b.ceilingDb = toneDb - 10.0f;
        check (removed (rms (run (b, 0.0f))), "a ceiling 10 dB under the tone removes it");
    }

    // ---- 5. empty window ---------------------------------------------------
    {
        auto b = fullBand();
        b.gateDb    = toneDb - 20.0f;
        b.ceilingDb = toneDb - 30.0f;
        check (removed (rms (run (b, 0.0f))), "a ceiling under the gate passes nothing");
    }

    // ---- 6. knee -----------------------------------------------------------
    {
        // Gate 3 dB over the centre bin: a hard gate removes all three bins,
        // a 24 dB knee lets part of each through.
        auto b = fullBand();
        b.gateDb = toneDb + 3.0f;
        check (removed (rms (run (b, 0.0f))), "hard gate just over the tone removes it");

        const double soft = rms (run (b, 24.0f));
        check (soft > 0.05 * ref && soft < 0.9 * ref, "a 24 dB knee on the gate gives a partial gain");

        // Ceiling 3 dB under the centre bin: the knee attenuates without
        // removing.
        auto c = fullBand();
        c.ceilingDb = toneDb - 3.0f;
        const double softCeiling = rms (run (c, 24.0f));
        check (softCeiling > 0.05 * ref && softCeiling < 0.9 * ref,
               "a 24 dB knee on the ceiling gives a partial gain");

        // Setting a knee and taking it back must leave exactly the hard switch
        // of a splitter whose knee was never touched. The gate sits 6 dB under
        // the centre bin, on the neighbours' level, so a leftover knee would
        // show.
        auto d = fullBand();
        d.gateDb = toneDb - 6.0f;
        check (run (d, 0.0f, 12.0f) == run (d, -1.0f),
               "a knee set then taken back to zero is exactly the untouched hard switch");
    }

    // ---- 7. pan on or off --------------------------------------------------
    {
        auto b = fullBand();
        b.pan = -1.0f;

        const auto panned = runStereo (b, true);
        check (rms (panned.second) < 0.01 * rms (panned.first),
               "with the pan on, a hard-left band leaves the right channel silent");

        const auto mono = runStereo (b, false);
        check (mono.first == mono.second,
               "with the pan off, both channels carry the same band, sample for sample");

        const double unity = amplitude / std::sqrt (2.0);
        check (std::abs (rms (mono.first) - unity) < 0.05 * unity,
               "with the pan off, the band comes out at its own gain (unity here)");
    }

    // ---- 8. gate openness ---------------------------------------------------
    {
        char what[160];
        auto b = fullBand();
        check (opennessAfter (b) == 1.0f, "openness is 1 with no level gating");

        b.gateDb = toneDb - 20.0f;
        const float passing = opennessAfter (b);
        std::snprintf (what, sizeof what,
                       "openness is near 1 when a gate under the tone lets it through (%.3f)", passing);
        check (passing > 0.9f, what);

        b.gateDb = toneDb + 10.0f;
        const float closed = opennessAfter (b);
        std::snprintf (what, sizeof what,
                       "openness is near 0 when a gate over the tone removes it (%.3f)", closed);
        check (closed < 0.1f, what);

        auto off = fullBand();
        off.enabled = false;
        check (opennessAfter (off) == 0.0f, "openness is 0 for a disabled band");
    }

    // ---- 9. the analyser reads the gate's level ------------------------------
    for (int analysedOrder = 10; analysedOrder <= 14; ++analysedOrder)
    {
        const int n = 1 << analysedOrder;
        const int bin = n / 32;                  // 1500 Hz at every size
        const double w = 2.0 * 3.141592653589793238 * bin / (double) n;

        fxme::SpectrumTap tap;
        tap.setEnabled (true);
        std::vector<float> tone ((size_t) (2 * n));
        for (size_t i = 0; i < tone.size(); ++i)
            tone[i] = amplitude * (float) std::sin (w * (double) i);
        tap.push (tone.data(), (int) tone.size());

        fxme::SpectrumAnalyzer analyser;
        analyser.setFftSize (n);
        std::array<float, fxme::SpectrumAnalyzer::numPoints> db;
        db.fill (-120.0f);
        analyser.update (tap, db, sampleRate, fxme::SpectrumAnalyzer::Mode::peak, 1.0f);

        const float peak = *std::max_element (db.begin(), db.end());
        char what[160];
        std::snprintf (what, sizeof what,
                       "at %d points the analyser reads the tone at the gate's level (%+.4f dB off)",
                       n, peak - toneDb);
        check (std::abs (peak - toneDb) < 0.001f, what);
    }

    std::printf ("\n%s (%d failures)\n",
                 failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
