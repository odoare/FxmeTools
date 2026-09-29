/*
  ------------------------------------------------------------------------------
    CoreDelayInsertTests.cpp

    What an insert inside a delay loop needs:

      1. fxme::StereoCrossDelay with an insert that does nothing is the plain
         delay, sample for sample.
      2. The insert acts on what enters the lines: with no feedback it scales
         the first echo; with feedback it acts again on every repeat.
      3. fxme::SplicePitchShifter at 0 semitones: a clean delay of
         getLatencySamples(), no splices.
      4. At +-12 and +-7 semitones, on tones at several frequencies, the
         target frequency is the peak of the output's spectrum, and not
         cancelled (the aligned splices keep a tone in phase; a free-running
         dual-tap shifter spreads it into a comb of lines around the
         target, sometimes with the target itself 60 dB down).
      5. Stereo: two identical channels come out identical (one set of
         splices for both).

    Exit code 0 when everything passes.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/dsp/SplicePitchShifter.h>
#include <FxmeTools/dsp/StereoCrossDelay.h>

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

    /** The left output of a 10 ms delay fed one impulse, over 40 ms. */
    std::vector<float> echoes (float feedback, float insertGain)
    {
        fxme::StereoCrossDelay delay;
        delay.prepare (sampleRate, 1.0f);
        delay.setDelaySeconds (0.01f, 0.01f);
        delay.setFeedback (feedback, feedback, 0.0f);
        delay.reset();

        const int n = (int) (0.04 * sampleRate);
        std::vector<float> out ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            const float x = i == 0 ? 1.0f : 0.0f;
            float l = 0.0f, r = 0.0f;
            delay.processSample (x, x, l, r, [insertGain] (float& a, float& b) noexcept
            {
                a *= insertGain;
                b *= insertGain;
            });
            out[(size_t) i] = l;
        }
        return out;
    }

    /** The largest output within a few samples of echo `k` (k * 10 ms). */
    float echoPeak (const std::vector<float>& out, int k)
    {
        const int centre = (int) (k * 0.01 * sampleRate);
        float peak = 0.0f;
        for (int i = std::max (0, centre - 4); i <= centre + 4 && i < (int) out.size(); ++i)
            peak = std::max (peak, std::abs (out[(size_t) i]));
        return peak;
    }

    /** The strongest frequency of a signal from `from` on, by a Goertzel
        scan in 1 Hz steps. Zero crossings would be thrown off by the phase
        jump at each of the shifter's splices; the spectral peak is not. */
    double frequencyOf (const std::vector<float>& x, int from)
    {
        double best = 0.0, bestPower = -1.0;
        for (double f = 60.0; f <= 1200.0; f += 0.5)
        {
            const double coef = 2.0 * std::cos (2.0 * pi * f / sampleRate);
            double s1 = 0.0, s2 = 0.0;
            for (int i = from; i < (int) x.size(); ++i)
            {
                const double s0 = x[(size_t) i] + coef * s1 - s2;
                s2 = s1;
                s1 = s0;
            }
            const double power = s1 * s1 + s2 * s2 - coef * s1 * s2;
            if (power > bestPower)
            {
                bestPower = power;
                best = f;
            }
        }
        return best;
    }
}

int main()
{
    std::printf ("Delay loop insert and pitch shifter\n");
    char what[200];

    // ---- 1. a neutral insert changes nothing -----------------------------------
    {
        fxme::StereoCrossDelay a, b;
        for (auto* d : { &a, &b })
        {
            d->prepare (sampleRate, 1.0f);
            d->setDelaySeconds (0.013f, 0.021f);
            d->setFeedback (0.6f, 0.4f, 0.3f);
            d->setDamping (0.3f);
        }

        float worst = 0.0f;
        for (int i = 0; i < (int) sampleRate / 2; ++i)
        {
            const float xl = (float) std::sin (0.01 * i), xr = (float) std::cos (0.017 * i);
            float al, ar, bl, br;
            a.processSample (xl, xr, al, ar);
            b.processSample (xl, xr, bl, br, [] (float&, float&) noexcept {});
            worst = std::max ({ worst, std::abs (al - bl), std::abs (ar - br) });
        }
        check (worst == 0.0f, "an insert that does nothing leaves the delay unchanged, sample for sample");
    }

    // ---- 2. the insert is on the write path -------------------------------------
    {
        const auto plain = echoes (0.0f, 0.5f);
        std::snprintf (what, sizeof what, "no feedback: the insert halves the first echo (%.3f)", echoPeak (plain, 1));
        check (std::abs (echoPeak (plain, 1) - 0.5f) < 0.02f, what);

        // Echo 2 went through the insert twice and the feedback once.
        const auto fed = echoes (0.5f, 0.5f);
        std::snprintf (what, sizeof what, "with feedback, every repeat goes through it again (%.3f, %.4f)",
                       echoPeak (fed, 1), echoPeak (fed, 2));
        check (std::abs (echoPeak (fed, 1) - 0.5f) < 0.02f
                   && std::abs (echoPeak (fed, 2) - 0.125f) < 0.01f, what);
    }

    // ---- 3. unity: a clean delay ---------------------------------------------------
    {
        fxme::SplicePitchShifter shifter;
        shifter.prepare (sampleRate, 1);
        shifter.setPitchSemitones (0.0f);
        shifter.reset();

        const int n = (int) sampleRate;
        std::vector<float> in ((size_t) n), out ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            in[(size_t) i] = 0.5f * (float) std::sin (2.0 * pi * 1234.0 * i / sampleRate);
            out[(size_t) i] = shifter.processSample (in[(size_t) i]);
        }

        const int lag = (int) std::lround (shifter.getLatencySamples());
        float worst = 0.0f;
        for (int i = lag; i < n; ++i)
            worst = std::max (worst, std::abs (out[(size_t) i] - in[(size_t) (i - lag)]));
        std::snprintf (what, sizeof what,
                       "0 semitones: the input %d samples late (max error %.2g)", lag, worst);
        check (worst < 1.0e-5f, what);
    }

    // ---- 4. the shift lands on the target -------------------------------------------
    for (float semis : { 12.0f, -12.0f, 7.0f, -7.0f })
        for (float fin : { 220.0f, 440.0f, 523.0f })
        {
            fxme::SplicePitchShifter shifter;
            shifter.prepare (sampleRate, 1);
            shifter.setPitchSemitones (semis);
            shifter.reset();

            const int n = 2 * (int) sampleRate;
            std::vector<float> out ((size_t) n);
            for (int i = 0; i < n; ++i)
                out[(size_t) i] = shifter.processSample (0.5f * (float) std::sin (2.0 * pi * fin * i / sampleRate));

            const double expected = fin * std::exp2 (semis / 12.0);
            const double f = frequencyOf (out, n / 4);
            std::snprintf (what, sizeof what, "%+3.0f semitones, %3.0f Hz: peak at %.1f Hz (target %.1f)",
                           semis, fin, f, expected);
            check (std::abs (f / expected - 1.0) < 0.005, what);
        }

    // ---- 5. stereo: one set of splices ------------------------------------------------
    {
        fxme::SplicePitchShifter shifter;
        shifter.prepare (sampleRate, 2);
        shifter.setPitchSemitones (5.0f);
        shifter.reset();

        float worst = 0.0f;
        for (int i = 0; i < (int) sampleRate; ++i)
        {
            const float x = 0.4f * (float) std::sin (2.0 * pi * 330.0 * i / sampleRate)
                          + 0.2f * (float) std::sin (2.0 * pi * 1210.0 * i / sampleRate);
            float frame[2] { x, x };
            shifter.processFrame (frame);
            worst = std::max (worst, std::abs (frame[0] - frame[1]));
        }
        check (worst == 0.0f, "identical channels come out identical");
    }

    std::printf ("\n%s (%d failures)\n",
                 failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
