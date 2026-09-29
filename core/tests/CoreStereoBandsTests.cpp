/*
  ------------------------------------------------------------------------------
    CoreStereoBandsTests.cpp

    Stereo input through both splitters and the spectral effects, where every
    decision is taken on the channels' average (the mono sum):

      1. Spectral splitter, identical channels: each band's left and right are
         exactly the mono splitter's band, gates and all.
      2. Spectral splitter, one channel only: the other side stays silent, and
         the gate compares the sum (half the channel), so a line between the
         channel's level and the sum's closes the band.
      3. Spectral effects, identical channels: a pitch shift gives exactly
         the mono result on both sides.
      4. Spectral effects keep the image: a source twice as loud on the left
         (and delayed on the right) comes out of a pitch shift still twice as
         loud on the left, the two sides still correlated.
      5. Filter bank, identical channels: both sides are the mono band; one
         channel only: the gate follows the sum.
      6. Balance: with the pan applied, a stereo band panned right keeps its
         right side as it is and turns the left down.

    Exit code 0 when everything passes.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/dsp/FilterBankSplitter.h>
#include <FxmeTools/dsp/SpectralBandEffects.h>
#include <FxmeTools/dsp/SpectralBandSplitter.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
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
    constexpr int    block      = 512;
    constexpr int    numBlocks  = 200;

    std::vector<float> signal (unsigned seed)
    {
        std::mt19937 rng (seed);
        std::normal_distribution<float> noise (0.0f, 0.05f);
        std::vector<float> x ((size_t) (block * numBlocks));
        for (size_t i = 0; i < x.size(); ++i)
            x[i] = 0.3f * (float) std::sin (2.0 * pi * 440.0 * (double) i / sampleRate)
                 + 0.1f * (float) std::sin (2.0 * pi * 1250.0 * (double) i / sampleRate) + noise (rng);
        return x;
    }

    double rms (const std::vector<float>& x, size_t from)
    {
        double s = 0.0;
        for (size_t i = from; i < x.size(); ++i)
            s += (double) x[i] * x[i];
        return std::sqrt (s / (double) (x.size() - from));
    }

    /** Runs a two-band spectral splitter, optionally with pitch-shifting
        effects, over the given channels (right null: mono), and collects
        band 0's left and right. */
    void runSpectral (const std::vector<float>& left, const std::vector<float>* right,
                      float gateDb, float semitones,
                      std::vector<float>& outL, std::vector<float>& outR)
    {
        fxme::SpectralBandSplitter splitter;
        fxme::SpectralBandEffects effects;
        splitter.prepare (sampleRate, block, 2, 11, right != nullptr ? 2 : 1);
        splitter.setApplyPan (false);
        effects.prepare (splitter.getFftSize(), splitter.getHopSize(), sampleRate, 2);
        if (semitones != 0.0f)
        {
            fxme::SpectralBandEffects::Settings s;
            s.pitchOn = true;
            s.pitchSemitones = semitones;
            effects.setBand (0, s);
            splitter.setBandProcessor (&effects);
        }
        splitter.setBand (0, { true, 200.0f, 3000.0f, gateDb, 0.0f, 0.0f });
        splitter.setBand (1, { true, 3000.0f, 9000.0f, -1000.0f, 0.0f, 0.0f });
        splitter.reset();

        outL.clear();
        outR.clear();
        for (int b = 0; b < numBlocks; ++b)
        {
            const float* l = left.data() + b * block;
            if (right != nullptr)
                splitter.process (l, right->data() + b * block, block);
            else
                splitter.process (l, block);

            outL.insert (outL.end(), splitter.getBandOutput (0, 0), splitter.getBandOutput (0, 0) + block);
            outR.insert (outR.end(), splitter.getBandOutput (0, 1), splitter.getBandOutput (0, 1) + block);
        }
    }

    float maxDiff (const std::vector<float>& a, const std::vector<float>& b)
    {
        float d = 0.0f;
        for (size_t i = 0; i < a.size() && i < b.size(); ++i)
            d = std::max (d, std::abs (a[i] - b[i]));
        return d;
    }
}

int main()
{
    std::printf ("Stereo bands\n");
    char what[200];
    const auto x = signal (1);
    const size_t settled = (size_t) (block * numBlocks / 2);

    // ---- 1. identical channels = mono ---------------------------------------------
    {
        std::vector<float> monoL, monoR, stL, stR;
        runSpectral (x, nullptr, -30.0f, 0.0f, monoL, monoR);
        runSpectral (x, &x, -30.0f, 0.0f, stL, stR);
        const float d = std::max (maxDiff (monoL, stL), maxDiff (monoL, stR));
        std::snprintf (what, sizeof what, "spectral, identical channels: both sides are the mono band (max difference %g)", d);
        check (d == 0.0f, what);
    }

    // ---- 2. one channel only -------------------------------------------------------
    {
        const std::vector<float> silence (x.size(), 0.0f);
        std::vector<float> l, r, monoL, monoR;
        runSpectral (x, &silence, -1000.0f, 0.0f, l, r);
        runSpectral (x, nullptr, -1000.0f, 0.0f, monoL, monoR);
        check (rms (r, settled) == 0.0 && maxDiff (l, monoL) < 1.0e-6f,
               "spectral, left only, gate open: the left is the mono band, the right silent");

        // The 440 Hz tone reads 20 log10 (0.3 / 2) = -16.5 dB on its own and
        // 6 dB less in the sum: a gate at -19 dB passes it alone, not in stereo.
        runSpectral (x, nullptr, -19.0f, 0.0f, monoL, monoR);
        runSpectral (x, &silence, -19.0f, 0.0f, l, r);
        std::snprintf (what, sizeof what,
                       "spectral, the gate compares the sum: passes mono (%.3f), closes on left only (%.4f)",
                       rms (monoL, settled), rms (l, settled));
        check (rms (monoL, settled) > 0.1 && rms (l, settled) < 0.1 * rms (monoL, settled), what);
    }

    // ---- 3. effects, identical channels = mono ---------------------------------------
    {
        std::vector<float> monoL, monoR, stL, stR;
        runSpectral (x, nullptr, -1000.0f, 7.0f, monoL, monoR);
        runSpectral (x, &x, -1000.0f, 7.0f, stL, stR);
        const float d = std::max (maxDiff (monoL, stL), maxDiff (monoL, stR));
        std::snprintf (what, sizeof what, "effects, identical channels: pitch shift equals mono on both sides (max difference %g)", d);
        check (d < 1.0e-5f, what);
    }

    // ---- 4. effects keep the image -----------------------------------------------------
    {
        // Right: half the left, 0.3 ms later (a panned, slightly wide source).
        std::vector<float> right (x.size(), 0.0f);
        const int lag = 14;
        for (size_t i = (size_t) lag; i < x.size(); ++i)
            right[i] = 0.5f * x[i - (size_t) lag];

        std::vector<float> l, r;
        runSpectral (x, &right, -1000.0f, 5.0f, l, r);
        const double balance = 20.0 * std::log10 (rms (l, settled) / rms (r, settled));

        double cross = 0.0, el = 0.0, er = 0.0;
        for (size_t i = settled; i + (size_t) lag < l.size(); ++i)
        {
            cross += (double) l[i] * r[i + (size_t) lag];
            el += (double) l[i] * l[i];
            er += (double) r[i + (size_t) lag] * r[i + (size_t) lag];
        }
        const double correlation = cross / std::sqrt (el * er);

        std::snprintf (what, sizeof what,
                       "effects keep the image: left %.2f dB over right (6.02 in), correlation %.3f",
                       balance, correlation);
        check (std::abs (balance - 6.02) < 0.5 && correlation > 0.9, what);
    }

    // ---- 5. filter bank ----------------------------------------------------------------
    {
        fxme::FilterBankSplitter mono, stereo, leftOnly;
        mono.prepare (sampleRate, block, 1, 1);
        stereo.prepare (sampleRate, block, 1, 2);
        leftOnly.prepare (sampleRate, block, 1, 2);
        const float toneDb = 20.0f * std::log10 (0.3f / 2.0f);
        for (auto* b : { &mono, &stereo, &leftOnly })
        {
            b->setApplyPan (false);
            b->setBand (0, { true, 300.0f, 700.0f, toneDb - 4.0f, 0.0f, 0.0f });
            b->reset();
        }

        const std::vector<float> silence (x.size(), 0.0f);
        float d = 0.0f;
        double leftOnlyEnergy = 0.0, monoEnergy = 0.0;
        for (int b = 0; b < numBlocks; ++b)
        {
            const float* in = x.data() + b * block;
            mono.process (in, block);
            stereo.process (in, in, block);
            leftOnly.process (in, silence.data() + b * block, block);
            for (int i = 0; i < block; ++i)
            {
                d = std::max ({ d, std::abs (mono.getBandOutput (0, 0)[i] - stereo.getBandOutput (0, 0)[i]),
                                   std::abs (mono.getBandOutput (0, 0)[i] - stereo.getBandOutput (0, 1)[i]) });
                if ((size_t) (b * block + i) >= settled)
                {
                    monoEnergy     += (double) mono.getBandOutput (0, 0)[i] * mono.getBandOutput (0, 0)[i];
                    leftOnlyEnergy += (double) leftOnly.getBandOutput (0, 0)[i] * leftOnly.getBandOutput (0, 0)[i];
                }
            }
        }
        std::snprintf (what, sizeof what, "filter bank, identical channels: both sides are the mono band (max difference %g)", d);
        check (d < 1.0e-6f, what);
        check (monoEnergy > 0.0 && leftOnlyEnergy < 0.01 * monoEnergy,
               "filter bank, the gate follows the sum: open in mono, closed on left only");
    }

    // ---- 6. balance -----------------------------------------------------------------------
    {
        fxme::SpectralBandSplitter splitter;
        splitter.prepare (sampleRate, block, 1, 11, 2);
        splitter.setBand (0, { true, 200.0f, 3000.0f, -1000.0f, 0.0f, 0.5f });
        splitter.reset();

        std::vector<float> l, r;
        for (int b = 0; b < numBlocks; ++b)
        {
            splitter.process (x.data() + b * block, x.data() + b * block, block);
            l.insert (l.end(), splitter.getBandOutput (0, 0), splitter.getBandOutput (0, 0) + block);
            r.insert (r.end(), splitter.getBandOutput (0, 1), splitter.getBandOutput (0, 1) + block);
        }
        const double ratio = rms (l, settled) / rms (r, settled);
        std::snprintf (what, sizeof what,
                       "balance at +0.5: right as it is, left at cos (pi/4) = 0.707 of it (%.3f)", ratio);
        check (std::abs (ratio - std::cos (pi / 4.0)) < 0.01, what);
    }

    std::printf ("\n%s (%d failures)\n",
                 failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
