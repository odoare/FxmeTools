/*
  ------------------------------------------------------------------------------
    CoreFilterBankTests.cpp

    fxme::BandGate and fxme::FilterBankSplitter, the live (filter bank)
    engine:

      1. Level convention: a steady sine of amplitude A reads A/2, the level
         the spectral gate and the spectrum view give its centre bin.
      2. The gate passes a tone above its line and removes one below it; the
         ceiling mirrors it; a knee gives a partial gain in between.
      3. Attack and release: the gain covers 63 % of its way in about the
         time set (per sample, so a 1 ms attack means 1 ms).
      4. The splitter, gate open, pan off, 0 dB: each band is exactly
         fxme::EdgeBandPass on the input, sample for sample.
      5. No latency: a band spanning the audible range passes an impulse
         through at once, untouched.
      6. Openness and level reporting: the gate gain, 1 with no gating, 0 for
         a disabled band; the band level as the gate compares it.

    Exit code 0 when everything passes.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/dsp/BandGate.h>
#include <FxmeTools/dsp/FilterBankSplitter.h>

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
    constexpr float  amplitude  = 0.5f;
    constexpr float  toneHz     = 1000.0f;

    /** The tone's level in the gate's convention: amplitude / 2. */
    const float toneDb = 20.0f * std::log10 (amplitude * 0.5f);

    double db (double x) { return 20.0 * std::log10 (std::max (x, 1.0e-12)); }

    std::vector<float> tone (int n, float a = amplitude, float hz = toneHz)
    {
        std::vector<float> x ((size_t) n);
        for (int i = 0; i < n; ++i)
            x[(size_t) i] = a * (float) std::sin (2.0 * pi * hz * i / sampleRate);
        return x;
    }

    double rms (const float* x, int n)
    {
        double s = 0.0;
        for (int i = 0; i < n; ++i)
            s += (double) x[i] * x[i];
        return n > 0 ? std::sqrt (s / n) : 0.0;
    }

    /** One second of the tone through a gate; RMS over the last quarter. */
    double gatedRms (float gateDb, float ceilingDb, float kneeDb = 0.0f)
    {
        fxme::BandGate gate;
        gate.prepare (sampleRate);
        gate.setKnee (kneeDb);
        gate.setThresholds (gateDb, ceilingDb);
        auto x = tone ((int) sampleRate);
        gate.process (x.data(), (int) x.size());
        const int from = (int) x.size() * 3 / 4;
        return rms (x.data() + from, (int) x.size() - from);
    }

    /** Samples for the gain to cover 63 % of its way after the tone starts
        (attack) or stops (release), with a detector short enough not to
        matter. */
    double timeConstantMs (bool attack, float seconds)
    {
        fxme::BandGate gate;
        gate.prepare (sampleRate);
        gate.setDetectorSeconds (0.0002f);
        gate.setTimes (attack ? seconds : 0.0f, attack ? 0.0f : seconds);
        // Just under the level of the DC used below, so the detector crosses
        // it almost at once and only the gain's own time is measured.
        gate.setThresholds (toneDb, fxme::BandGate::offCeilingDb);

        // DC over the gate (a steady level, no ripple) for a release, silence
        // for an attack: the gain starts from the other end.
        std::vector<float> x (1, 0.0f);
        for (int i = 0; i < (int) sampleRate / 2; ++i)
        {
            x[0] = attack ? 0.0f : amplitude;
            gate.process (x.data(), 1);
        }

        for (int i = 0; i < (int) sampleRate; ++i)
        {
            x[0] = attack ? amplitude : 0.0f;
            gate.process (x.data(), 1);
            const float g = gate.getGain();
            if (attack ? g >= 1.0f - std::exp (-1.0f) : g <= std::exp (-1.0f))
                return 1000.0 * i / sampleRate;
        }
        return -1.0;
    }
}

int main()
{
    std::printf ("BandGate and FilterBankSplitter\n");
    const double inRms = amplitude / std::sqrt (2.0);
    char what[200];

    // ---- 1. level convention ------------------------------------------------
    {
        fxme::BandGate gate;
        gate.prepare (sampleRate);
        auto x = tone ((int) sampleRate / 2);
        gate.process (x.data(), (int) x.size());
        std::snprintf (what, sizeof what, "a sine reads at its spectrum level (%+.2f dB off)",
                       gate.getLevelDb() - toneDb);
        check (std::abs (gate.getLevelDb() - toneDb) < 0.2f, what);
    }

    // ---- 2. gate, ceiling, knee -----------------------------------------------
    {
        const float off = fxme::BandGate::offCeilingDb, open = fxme::BandGate::openGateDb;
        check (std::abs (db (gatedRms (toneDb - 10.0f, off)) - db (inRms)) < 0.1,
               "a gate 10 dB under the tone passes it");
        check (db (gatedRms (toneDb + 10.0f, off)) < db (inRms) - 60.0,
               "a gate 10 dB over the tone removes it");
        check (std::abs (db (gatedRms (open, toneDb + 10.0f)) - db (inRms)) < 0.1,
               "a ceiling 10 dB over the tone passes it");
        check (db (gatedRms (open, toneDb - 10.0f)) < db (inRms) - 60.0,
               "a ceiling 10 dB under the tone removes it");
        check (db (gatedRms (toneDb - 5.0f, toneDb - 10.0f)) < db (inRms) - 60.0,
               "a ceiling under the gate passes nothing");

        const double soft = gatedRms (toneDb + 2.0f, off, 24.0f);
        check (soft > 0.05 * inRms && soft < 0.9 * inRms,
               "a 24 dB knee on the gate gives a partial gain");
        check (std::abs (db (gatedRms (open, off)) - db (inRms)) < 0.001,
               "no gate and no ceiling: untouched");
    }

    // ---- 3. attack and release -------------------------------------------------
    for (float ms : { 1.0f, 20.0f })
    {
        const double a = timeConstantMs (true, ms * 0.001f);
        std::snprintf (what, sizeof what, "attack %.0f ms reaches 63 %% in %.2f ms", ms, a);
        check (std::abs (a - ms) < 0.1 * ms + 0.1, what);
    }
    for (float ms : { 10.0f, 200.0f })
    {
        const double r = timeConstantMs (false, ms * 0.001f);
        std::snprintf (what, sizeof what, "release %.0f ms falls 63 %% in %.2f ms", ms, r);
        check (std::abs (r - ms) < 0.1 * ms + 0.1, what);
    }

    // ---- 4. a band is EdgeBandPass on the input ---------------------------------
    {
        constexpr int block = 256;
        fxme::FilterBankSplitter bank;
        bank.prepare (sampleRate, block, 2);
        bank.setApplyPan (false);
        bank.setBand (0, { true, 200.0f, 2000.0f, -1000.0f, 0.0f, 0.0f });
        bank.setBand (1, { true, 3000.0f, 9000.0f, -1000.0f, 0.0f, 0.0f });
        bank.reset();   // gains straight to their targets, no 20 ms fade-in

        fxme::EdgeBandPass ref[2];
        for (auto& r : ref)
            r.prepare (sampleRate);
        ref[0].setEdges (200.0f, 2000.0f, true);
        ref[1].setEdges (3000.0f, 9000.0f, true);

        auto in = tone (block * 40, 0.3f, 700.0f);
        const auto second = tone (block * 40, 0.3f, 5000.0f);
        for (size_t i = 0; i < in.size(); ++i)
            in[i] += second[i];

        double worst = 0.0;
        for (int start = 0; start + block <= (int) in.size(); start += block)
        {
            bank.process (in.data() + start, block);
            for (int b = 0; b < 2; ++b)
            {
                std::vector<float> r (in.begin() + start, in.begin() + start + block);
                ref[b].process (r.data(), block);
                for (int i = 0; i < block; ++i)
                    for (int ch = 0; ch < 2; ++ch)
                        worst = std::max (worst, (double) std::abs (bank.getBandOutput (b, ch)[i] - r[(size_t) i]));
            }
        }
        std::snprintf (what, sizeof what, "each band is EdgeBandPass on the input (max difference %.2g)", worst);
        check (worst < 1.0e-6, what);
    }

    // ---- 5. no latency -----------------------------------------------------------
    {
        fxme::FilterBankSplitter bank;
        bank.prepare (sampleRate, 64, 1);
        bank.setApplyPan (false);
        bank.setBand (0, { true, 20.0f, 20000.0f, -1000.0f, 0.0f, 0.0f });
        bank.reset();
        std::vector<float> impulse (64, 0.0f);
        impulse[0] = 1.0f;
        bank.process (impulse.data(), 64);
        check (bank.getLatencySamples() == 0 && bank.getBandOutput (0, 0)[0] == 1.0f
                   && bank.getBandOutput (0, 0)[1] == 0.0f,
               "a band spanning the audible range passes an impulse at once, untouched");
    }

    // ---- 6. openness and level ---------------------------------------------------
    {
        fxme::FilterBankSplitter bank;
        bank.prepare (sampleRate, 512, 3);
        bank.setBand (0, { true, 500.0f, 2000.0f, -1000.0f, 0.0f, 0.0f });           // open
        bank.setBand (1, { true, 500.0f, 2000.0f, toneDb + 10.0f, 0.0f, 0.0f });     // closed
        bank.setBand (2, { false, 500.0f, 2000.0f, -1000.0f, 0.0f, 0.0f });          // disabled
        const auto x = tone ((int) sampleRate);
        for (int start = 0; start + 512 <= (int) x.size(); start += 512)
            bank.process (x.data() + start, 512);

        check (bank.getGateOpenness (0) == 1.0f, "openness is 1 with no level gating");
        check (bank.getGateOpenness (1) < 0.01f, "openness is near 0 when the gate removes the band");
        check (bank.getGateOpenness (2) == 0.0f, "openness is 0 for a disabled band");
        std::snprintf (what, sizeof what, "the band level reads the tone (%+.2f dB off)",
                       bank.getBandLevelDb (1) - toneDb);
        check (std::abs (bank.getBandLevelDb (1) - toneDb) < 0.5f, what);
    }

    std::printf ("\n%s (%d failures)\n",
                 failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
