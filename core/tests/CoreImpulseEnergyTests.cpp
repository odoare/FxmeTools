/*
  ------------------------------------------------------------------------------
    CoreImpulseEnergyTests.cpp

    fxme::ImpulseEnergy:

      1. Energy is the sum of squares, averaged over channels.
      2. normalise() brings any IR to unit energy (or a given target).
      3. Convolving noise with two very different IRs, a short loud click
         and a long dense decaying tail, gives the same output RMS once both
         are normalised (and wildly different RMS before).
      4. A stereo IR keeps its left / right balance (one gain).
      5. A silent IR, and empty input, are left alone.
      6. The pink-weighted measure: 1 for a unit impulse at any sample rate;
         and on an IR that boosts the lows (as a forest reverb does), pink
         noise comes out at its input level after normaliseLoudness(), where
         the flat normalise() leaves it several dB louder.

    Exit code 0 when everything passes.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/dsp/ImpulseEnergy.h>

#include <cmath>
#include <cstdint>
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
    /** Deterministic white noise in [-1, 1). */
    struct Noise
    {
        uint32_t state = 12345u;
        float next()
        {
            state = state * 1664525u + 1013904223u;
            return (float) ((double) (state >> 8) / (double) (1u << 24)) * 2.0f - 1.0f;
        }
    };

    std::vector<float> convolve (const std::vector<float>& x, const std::vector<float>& h)
    {
        std::vector<float> y (x.size() + h.size() - 1, 0.0f);
        for (size_t i = 0; i < x.size(); ++i)
            for (size_t k = 0; k < h.size(); ++k)
                y[i + k] += x[i] * h[k];
        return y;
    }

    /** RMS over [from, to) of y: the steady part, past the IR's own length. */
    double rms (const std::vector<float>& y, size_t from, size_t to)
    {
        double s = 0.0;
        for (size_t i = from; i < to; ++i)
            s += (double) y[i] * y[i];
        return std::sqrt (s / (double) (to - from));
    }

    double energyOf (std::vector<float>& h)
    {
        float* p = h.data();
        return fxme::ImpulseEnergy::meanChannelEnergy (&p, 1, (int) h.size());
    }

    bool near (double a, double b, double relative) { return std::abs (a - b) <= relative * std::abs (b); }
}

int main()
{
    std::printf ("ImpulseEnergy\n");

    // ---- 1. energy -------------------------------------------------------
    {
        std::vector<float> l { 1.0f, 0.0f, 0.5f }, r { 0.0f, 0.0f, 0.0f };
        const float* ch[] { l.data(), r.data() };
        check (near (fxme::ImpulseEnergy::meanChannelEnergy (ch, 2, 3), (1.0 + 0.25) / 2.0, 1e-9),
               "energy: sum of squares, averaged over the channels");
    }

    // ---- 2. unit energy, or a target ---------------------------------------
    {
        std::vector<float> h { 3.0f, -2.0f, 1.0f, 0.5f };
        float* p = h.data();
        const float g = fxme::ImpulseEnergy::normalise (&p, 1, (int) h.size());
        check (near (energyOf (h), 1.0, 1e-6), "normalise: unit energy");
        check (near ((double) g, 1.0 / std::sqrt (9.0 + 4.0 + 1.0 + 0.25), 1e-6), "and returns the gain applied");

        fxme::ImpulseEnergy::normalise (&p, 1, (int) h.size(), 0.25);
        check (near (energyOf (h), 0.25, 1e-6), "normalise: to a given target energy");
    }

    // ---- 3. equal loudness after convolution ------------------------------
    {
        Noise noise;
        std::vector<float> x (48000);
        for (auto& s : x)
            s = 0.5f * noise.next();

        // A short loud cabinet-like click, and a long dense reverb-like tail
        // (decaying noise, 0.5 s), whose raw energies differ hugely.
        std::vector<float> click { 0.9f, -0.6f, 0.3f, -0.1f };
        std::vector<float> tail (24000);
        Noise n2;
        n2.state = 777u;
        for (size_t i = 0; i < tail.size(); ++i)
            tail[i] = n2.next() * (float) std::exp (-6.9 * (double) i / (double) tail.size());

        const size_t from = tail.size(), to = x.size();
        const double rawClick = rms (convolve (x, click), from, to);
        const double rawTail  = rms (convolve (x, tail),  from, to);

        char what[160];
        std::snprintf (what, sizeof what, "before: the two IRs differ by %.1f dB",
                       20.0 * std::log10 (rawTail / rawClick));
        check (std::abs (20.0 * std::log10 (rawTail / rawClick)) > 10.0, what);

        float* pc = click.data();
        float* pt = tail.data();
        fxme::ImpulseEnergy::normalise (&pc, 1, (int) click.size());
        fxme::ImpulseEnergy::normalise (&pt, 1, (int) tail.size());
        const double outClick = rms (convolve (x, click), from, to);
        const double outTail  = rms (convolve (x, tail),  from, to);
        const double inRms    = rms (x, from, to);

        std::snprintf (what, sizeof what, "after: within 0.5 dB of each other (%.2f dB)",
                       20.0 * std::log10 (outTail / outClick));
        check (std::abs (20.0 * std::log10 (outTail / outClick)) < 0.5, what);
        std::snprintf (what, sizeof what, "and of the input's RMS (%.2f dB)",
                       20.0 * std::log10 (outTail / inRms));
        check (std::abs (20.0 * std::log10 (outTail / inRms)) < 0.5, what);
    }

    // ---- 4. stereo balance --------------------------------------------------
    {
        std::vector<float> l { 0.8f, 0.4f, 0.2f }, r { 0.4f, 0.2f, 0.1f };
        float* ch[] { l.data(), r.data() };
        fxme::ImpulseEnergy::normalise (ch, 2, 3);
        check (near ((double) l[0] / (double) r[0], 2.0, 1e-6), "stereo: one gain, the balance is kept");
        check (near (fxme::ImpulseEnergy::meanChannelEnergy (ch, 2, 3), 1.0, 1e-6),
               "stereo: unit mean energy");
    }

    // ---- 5. silent and empty -----------------------------------------------
    {
        std::vector<float> silent (64, 0.0f);
        float* p = silent.data();
        check (fxme::ImpulseEnergy::normalise (&p, 1, 64) == 1.0f && silent[0] == 0.0f,
               "a silent IR is left alone (gain 1)");
        check (fxme::ImpulseEnergy::normalise (nullptr, 0, 0) == 1.0f, "no channels: nothing done");
    }

    // ---- 6. pink-weighted loudness ------------------------------------------
    {
        char what[160];
        for (double sr : { 44100.0, 48000.0, 96000.0 })
        {
            std::vector<float> impulse (64, 0.0f);
            impulse[0] = 1.0f;
            const float* p = impulse.data();
            const double e = fxme::ImpulseEnergy::pinkWeightedEnergy (&p, 1, 64, sr);
            std::snprintf (what, sizeof what, "pink-weighted energy of a unit impulse at %.0f Hz is 1 (%.4f)", sr, e);
            check (near (e, 1.0, 0.01), what);
        }

        // Pink noise (Paul Kellet's filter on white noise), 1.5 s at 48 kHz.
        const double sr = 48000.0;
        Noise noise;
        std::vector<float> pink (72000);
        double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
        for (auto& s : pink)
        {
            const double w = noise.next();
            b0 = 0.99886 * b0 + w * 0.0555179;  b1 = 0.99332 * b1 + w * 0.0750759;
            b2 = 0.96900 * b2 + w * 0.1538520;  b3 = 0.86650 * b3 + w * 0.3104856;
            b4 = 0.55000 * b4 + w * 0.5329522;  b5 = -0.7616 * b5 - w * 0.0168980;
            s = (float) (0.1 * (b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362));
            b6 = w * 0.115926;
        }

        // Kellet's pink keeps going down to about 9 Hz; music (and the
        // measure, whose lowest octave starts at 22 Hz) does not: take the
        // infrasound out with two one-pole high-passes at 20 Hz.
        for (int pass = 0; pass < 2; ++pass)
        {
            const double c = std::exp (-2.0 * 3.14159265358979 * 20.0 / 48000.0);
            double prevIn = 0.0, prevOut = 0.0;
            for (auto& s : pink)
            {
                const double out = c * (prevOut + (double) s - prevIn);
                prevIn = s;
                prevOut = out;
                s = (float) out;
            }
        }

        // A dark, reverb-like IR: decaying noise through a one-pole low-pass
        // at about 300 Hz, so its lows are far above its highs.
        std::vector<float> dark (2400);
        Noise n3;
        n3.state = 4242u;
        double lp = 0.0;
        const double k = 1.0 - std::exp (-2.0 * 3.14159265358979 * 300.0 / sr);
        for (size_t i = 0; i < dark.size(); ++i)
        {
            lp += k * ((double) n3.next() - lp);
            dark[i] = (float) (lp * std::exp (-6.9 * (double) i / (double) dark.size()));
        }

        const size_t from = dark.size(), to = pink.size();
        const double inRms = rms (pink, from, to);

        std::vector<float> flat = dark, loud = dark;
        float* pf = flat.data();
        float* pl = loud.data();
        fxme::ImpulseEnergy::normalise (&pf, 1, (int) flat.size());
        fxme::ImpulseEnergy::normaliseLoudness (&pl, 1, (int) loud.size(), sr);

        const double flatDb = 20.0 * std::log10 (rms (convolve (pink, flat), from, to) / inRms);
        const double loudDb = 20.0 * std::log10 (rms (convolve (pink, loud), from, to) / inRms);

        std::snprintf (what, sizeof what, "dark IR, flat normalise: pink noise comes out %+.1f dB", flatDb);
        check (flatDb > 3.0, what);
        std::snprintf (what, sizeof what, "dark IR, normaliseLoudness: pink noise comes out %+.2f dB", loudDb);
        check (std::abs (loudDb) < 1.0, what);
    }

    std::printf ("\n%s (%d failures)\n", failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
