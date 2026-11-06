/*
  ------------------------------------------------------------------------------
    CoreCrusherTests.cpp

    fxme::BitCrusher and fxme::Downsampler:

      1. Mid-tread (the default): rounds to the step, silence stays silent.
      2. Mid-rise: levels at odd half steps, silence becomes +-half a step.
      3. Mu-law: full scale stays full scale, and a quiet signal comes out
         much closer than through the linear quantizer at the same depth.
      4. Dither: a constant between two levels averages to itself through a
         dithered quantizer (TPDF, +-1 step), not to the nearest level.
      5. Downsampler without jitter: holds of exactly `factor` samples, the
         first input latched at once (the behaviour before jitter existed).
      6. Jitter: hold lengths vary, average the factor, two instances with
         the same seed hold in lockstep, and reset() replays the sequence.

    Exit code 0 when everything passes.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/dsp/BitCrusher.h>
#include <FxmeTools/dsp/Downsampler.h>
#include <FxmeTools/util/Random.h>

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

static bool near (float a, float b, float tol = 1.0e-6f) { return std::abs (a - b) <= tol; }

/** Lengths of the runs of equal output values for `n` samples of a ramp. */
static std::vector<int> holdLengths (fxme::Downsampler& d, int n)
{
    std::vector<int> lengths;
    float last = -1.0f;
    for (int i = 0; i < n; ++i)
    {
        const float y = d.processSample ((float) i);
        if (y != last)
            lengths.push_back (0);
        ++lengths.back();
        last = y;
    }
    lengths.pop_back();   // the last run may be cut short
    return lengths;
}

int main()
{
    using Mode = fxme::BitCrusher::Mode;

    std::printf ("1. Mid-tread\n");
    {
        fxme::BitCrusher q;
        q.setBits (3.0f);   // 4 steps per unit: a step of 0.25
        check (near (q.getStep(), 0.25f), "3 bits: a step of 0.25");
        check (near (q.processSample (0.3f), 0.25f), "0.3 -> 0.25");
        check (near (q.processSample (-0.4f), -0.5f), "-0.4 -> -0.5");
        check (q.processSample (0.0f) == 0.0f, "0 stays 0");
    }

    std::printf ("2. Mid-rise\n");
    {
        fxme::BitCrusher q;
        q.setBits (3.0f);
        q.setMode (Mode::midRise);
        check (near (q.processSample (0.3f), 0.375f), "0.3 -> 0.375");
        check (near (q.processSample (0.0f), 0.125f), "0 -> half a step");
        check (near (q.processSample (-0.01f), -0.125f), "just under 0 -> minus half a step");
    }

    std::printf ("3. Mu-law\n");
    {
        fxme::BitCrusher lin, mu;
        lin.setBits (6.0f);
        mu.setBits (6.0f);
        mu.setMode (Mode::muLaw);
        check (near (mu.processSample (1.0f), 1.0f, 1.0e-5f), "+1 stays +1");
        check (near (mu.processSample (-1.0f), -1.0f, 1.0e-5f), "-1 stays -1");
        check (mu.processSample (0.0f) == 0.0f, "0 stays 0");

        double errLin = 0.0, errMu = 0.0;
        for (int i = 0; i < 1000; ++i)
        {
            const float x = 0.02f * (float) std::sin (0.01 * i);   // about -34 dBFS
            errLin += std::abs (lin.processSample (x) - x);
            errMu  += std::abs (mu.processSample (x) - x);
        }
        check (errMu < 0.25 * errLin, "quiet signal: mu-law error under a quarter of the linear one");
    }

    std::printf ("4. Dither\n");
    {
        fxme::BitCrusher q;
        q.setBits (4.0f);   // a step of 0.125
        fxme::Random rng (1234);
        const float x = 0.03f;   // a quarter of a step
        double sum = 0.0;
        const int n = 200000;
        for (int i = 0; i < n; ++i)
            sum += q.processSample (x, rng.nextFloat() - rng.nextFloat());
        check (q.processSample (x) == 0.0f, "undithered: rounds to 0");
        check (std::abs (sum / n - x) < 0.002, "dithered: averages to the input");
    }

    std::printf ("5. Downsampler, no jitter\n");
    {
        fxme::Downsampler d;
        d.setFactor (4.0f);
        d.reset();
        check (d.processSample (7.0f) == 7.0f, "the first input is latched at once");
        d.reset();
        const auto lengths = holdLengths (d, 400);
        bool allFour = ! lengths.empty();
        for (int l : lengths)
            allFour = allFour && l == 4;
        check (allFour, "every hold is 4 samples");
    }

    std::printf ("6. Jitter\n");
    {
        fxme::Downsampler a, b;
        for (auto* d : { &a, &b })
        {
            d->setFactor (8.0f);
            d->setJitter (0.5f, 42);
            d->reset();
        }
        const auto la = holdLengths (a, 80000);
        const auto lb = holdLengths (b, 80000);
        double mean = 0.0;
        int minL = 1 << 30, maxL = 0;
        for (int l : la)
        {
            mean += l;
            minL = l < minL ? l : minL;
            maxL = l > maxL ? l : maxL;
        }
        mean /= (double) la.size();
        check (minL < 8 && maxL > 8, "hold lengths vary around the factor");
        check (minL >= 3 && maxL <= 13, "within +-50 % (and rounding)");
        check (std::abs (mean - 8.0) < 0.3, "average hold close to the factor");
        check (la == lb, "same seed: lockstep");

        a.reset();
        check (holdLengths (a, 80000) == la, "reset() replays the sequence");
    }

    std::printf (failures == 0 ? "All crusher tests passed\n" : "%d crusher test(s) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
