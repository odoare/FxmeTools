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

    std::printf ("\n%s (%d failures)\n", failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
