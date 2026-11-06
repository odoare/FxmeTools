/*
  ------------------------------------------------------------------------------
    CoreTapeTests.cpp

    fxme::TapeHysteresis and fxme::tape (TapeEq.h):

      1. Emphasis: record then playback gives the input back (to rounding),
         at 44.1 and 96 kHz, NAB and IEC; the record curve's digital response
         matches its analog prototype at 1 kHz and 5 kHz.
      2. Head loss: -3 dB at headLossCutoffHz(), the cutoff rising with the
         speed (about 15-20 kHz at 7.5 ips).
      3. Hysteresis, bad input: NaN, infinities and huge values give finite
         output, and the model keeps working afterwards.
      4. Odd symmetry: -x in gives -M out.
      5. Low level: a quiet sine at the calibrated bias comes out nearly
         linear (THD under 0.5 %), and saturation keeps |M| under ~1.
      6. The loop: its area (the integral of M dH over a cycle) grows with
         the drive and shrinks with the bias.
      7. reset() is bit-exact: the same input gives the same output again.
      8. approxGain() is within 0.5 dB of the measured gain at the reference
         level, for every tape type and bias from 0.2 to 1.

    Exit code 0 when everything passes.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/dsp/TapeHysteresis.h>
#include <FxmeTools/dsp/TapeEq.h>
#include <FxmeTools/util/Random.h>

#include <cmath>
#include <complex>
#include <cstdio>
#include <limits>
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
    constexpr double pi = 3.141592653589793;
    using TapeType = fxme::TapeHysteresis::TapeType;

    std::vector<float> sine (double amp, double hz, double fs, int n)
    {
        std::vector<float> x ((size_t) n);
        for (int i = 0; i < n; ++i)
            x[(size_t) i] = (float) (amp * std::sin (2.0 * pi * hz * i / fs));
        return x;
    }

    std::vector<float> run (fxme::TapeHysteresis& h, const std::vector<float>& x)
    {
        std::vector<float> y (x.size());
        for (size_t i = 0; i < x.size(); ++i)
            y[i] = h.process (x[i]);
        return y;
    }

    double rms (const std::vector<float>& x, size_t from)
    {
        double s = 0.0;
        for (size_t i = from; i < x.size(); ++i)
            s += (double) x[i] * x[i];
        return std::sqrt (s / (double) (x.size() - from));
    }

    /** Amplitude of harmonic `h` of `f0` over the last `n` samples (one DFT bin). */
    double harmonic (const std::vector<float>& y, double f0, int h, double fs, size_t n)
    {
        std::complex<double> acc { 0.0, 0.0 };
        const size_t start = y.size() - n;
        for (size_t i = 0; i < n; ++i)
            acc += (double) y[start + i] * std::polar (1.0, -2.0 * pi * f0 * h * (double) i / fs);
        return 2.0 * std::abs (acc) / (double) n;
    }

    /** Loop area over the last cycle of an f0 sine of amplitude amp. */
    double loopArea (float bias, double amp)
    {
        const double fs = 96000.0, f0 = 100.0;
        const int period = (int) (fs / f0);
        fxme::TapeHysteresis h;
        h.prepare (fs);
        h.setParams (fxme::TapeHysteresis::paramsFor (TapeType::modern, bias));
        const auto x = sine (amp, f0, fs, period * 6);
        const auto y = run (h, x);
        double area = 0.0;
        for (size_t i = x.size() - (size_t) period; i < x.size(); ++i)
            area += 0.5 * (y[i] + y[i - 1]) * (x[i] - x[i - 1]);
        return std::abs (area);
    }
}

int main()
{
    using namespace fxme::tape;

    std::printf ("1. Emphasis\n");
    {
        bool exact = true;
        for (double fs : { 44100.0, 96000.0 })
            for (auto e : { EqStandard::nab, EqStandard::iec })
            {
                EmphasisFilter rec, play;
                rec.setup (Speed::ips15, e, true, fs);
                play.setup (Speed::ips15, e, false, fs);
                fxme::Random rng (7);
                for (int i = 0; i < 20000; ++i)
                {
                    const float x = rng.nextFloat() * 2.0f - 1.0f;
                    exact = exact && std::abs (play.process (rec.process (x)) - x) < 1.0e-4f;
                }
            }
        check (exact, "record then playback is the identity");

        // Digital record curve against the prototype, NAB 15 ips at 96 kHz.
        const double fs = 96000.0;
        bool matches = true;
        for (double hz : { 1000.0, 5000.0 })
        {
            EmphasisFilter rec;
            rec.setup (Speed::ips15, EqStandard::nab, true, fs);
            const auto x = sine (0.1, hz, fs, 48000);
            std::vector<float> y (x.size());
            for (size_t i = 0; i < x.size(); ++i)
                y[i] = rec.process (x[i]);
            const double gotDb = 20.0 * std::log10 (rms (y, 24000) / rms (x, 24000));
            const double wantDb = EmphasisFilter::recordMagnitudeDb (hz, Speed::ips15, EqStandard::nab);
            matches = matches && std::abs (gotDb - wantDb) < 0.3;
        }
        check (matches, "digital record curve within 0.3 dB of the prototype");
        check (EmphasisFilter::recordMagnitudeDb (10000.0, Speed::ips15, EqStandard::nab) > 8.0,
               "NAB pre-emphasis: highs boosted (over +8 dB at 10 kHz)");
        check (EmphasisFilter::recordMagnitudeDb (20.0, Speed::ips15, EqStandard::nab) > 5.0
               && std::abs (EmphasisFilter::recordMagnitudeDb (20.0, Speed::ips15, EqStandard::iec)) < 0.1,
               "NAB boosts the deep lows, IEC does not");
    }

    std::printf ("2. Head loss\n");
    {
        const double c7 = headLossCutoffHz (Speed::ips7_5), c15 = headLossCutoffHz (Speed::ips15),
                     c30 = headLossCutoffHz (Speed::ips30);
        std::printf ("     cutoffs: %.0f, %.0f, %.0f Hz\n", c7, c15, c30);
        check (std::abs (headLossDb (c7, Speed::ips7_5) + 3.0) < 0.01, "-3 dB at the cutoff");
        check (c7 < c15 && c15 < c30, "the cutoff rises with the speed");
        check (c7 > 12000.0 && c7 < 22000.0, "7.5 ips: between 12 and 22 kHz");
    }

    std::printf ("3. Bad input\n");
    {
        fxme::TapeHysteresis h;
        h.prepare (96000.0);
        const float bad[] { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                            -std::numeric_limits<float>::infinity(), 1.0e30f, -1.0e30f };
        bool finite = true;
        for (float b : bad)
            finite = finite && std::isfinite (h.process (b));
        const auto y = run (h, sine (0.5, 1000.0, 96000.0, 2000));
        for (float v : y)
            finite = finite && std::isfinite (v);
        check (finite, "finite output during and after bad input");
        check (rms (y, 1000) > 0.1, "still processing afterwards");
    }

    std::printf ("4. Odd symmetry\n");
    {
        fxme::TapeHysteresis a, b;
        a.prepare (96000.0);
        b.prepare (96000.0);
        const auto x = sine (0.8, 300.0, 96000.0, 5000);
        bool odd = true;
        for (float v : x)
            odd = odd && std::abs (a.process (v) + b.process (-v)) < 1.0e-6f;
        check (odd, "-x gives -M");
    }

    std::printf ("5. Low level and saturation\n");
    {
        const double fs = 96000.0, f0 = 1000.0;
        fxme::TapeHysteresis h;
        h.prepare (fs);
        h.setParams (fxme::TapeHysteresis::paramsFor (TapeType::modern, 0.5f));
        const auto y = run (h, sine (0.01, f0, fs, 19200));
        const size_t n = 9600;   // 100 cycles
        const double h1 = harmonic (y, f0, 1, fs, n);
        const double distortion = std::hypot (harmonic (y, f0, 2, fs, n), harmonic (y, f0, 3, fs, n),
                                              harmonic (y, f0, 5, fs, n));
        std::printf ("     THD at H = 0.01: %.3f %%\n", 100.0 * distortion / h1);
        check (distortion / h1 < 0.005, "quiet sine: THD under 0.5 %");

        h.reset();
        float peak = 0.0f;
        for (float v : run (h, sine (20.0, 200.0, fs, 4800)))
            peak = std::max (peak, std::abs (v));
        check (peak < 1.1f && peak > 0.8f, "driven hard: |M| saturates near 1");
    }

    std::printf ("6. The loop\n");
    {
        const double quiet = loopArea (0.5f, 0.3), hot = loopArea (0.5f, 1.0);
        const double under = loopArea (0.2f, 0.5), over = loopArea (0.9f, 0.5);
        std::printf ("     area: %.4f (0.3) -> %.4f (1.0); bias 0.2: %.4f, 0.9: %.4f\n", quiet, hot, under, over);
        check (hot > quiet, "the area grows with the drive");
        check (under > over, "and shrinks with the bias");
    }

    std::printf ("7. Reset\n");
    {
        fxme::TapeHysteresis h;
        h.prepare (96000.0);
        h.setParams (fxme::TapeHysteresis::paramsFor (TapeType::vintage, 0.3f));
        const auto x = sine (0.7, 440.0, 96000.0, 3000);
        const auto first = run (h, x);
        h.reset();
        check (run (h, x) == first, "bit-exact after reset()");
    }

    std::printf ("8. approxGain\n");
    {
        const double fs = 96000.0;
        double worst = 0.0;
        for (auto type : { TapeType::modern, TapeType::vintage, TapeType::highOutput })
            for (float bias : { 0.2f, 0.35f, 0.5f, 0.65f, 0.8f, 1.0f })
            {
                const auto p = fxme::TapeHysteresis::paramsFor (type, bias);
                fxme::TapeHysteresis h;
                h.prepare (fs);
                h.setParams (p);
                const auto x = sine (0.1, 200.0, fs, 5760);
                const auto y = run (h, x);
                const double measured = rms (y, 2880) / rms (x, 2880);
                worst = std::max (worst, std::abs (20.0 * std::log10 (measured / fxme::TapeHysteresis::approxGain (p))));
            }
        std::printf ("     worst error: %.2f dB\n", worst);
        check (worst < 0.5, "within 0.5 dB");
    }

    std::printf (failures == 0 ? "All tape tests passed\n" : "%d tape test(s) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
