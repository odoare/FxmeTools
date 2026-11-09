/*
  ------------------------------------------------------------------------------
    CoreResamplerTests.cpp

    fxme::Resampler and fxme::ResamplingContainer:

      1. Exact latency: through a container around an identity process, the
         output is the input delayed by exactly getLatencySamples() (a sine
         at 1 and 10 kHz under -80 dB of error), and one sample off is far
         worse, for every host rate from 44.1 to 192 kHz around 48 kHz.
      2. Flat passband: the round trip's gain at 20 kHz within 0.1 dB of its
         gain at 1 kHz (44.1 and 96 kHz hosts).
      3. Aliasing: what lies above the lower Nyquist does not fold back:
         96 -> 48 kHz with a 30 kHz tone, 48 -> 44.1 kHz with a 23 kHz tone,
         under -90 dB.
      4. Any block sizes: random host blocks from 1 to the maximum, no
         underrun, the inner process never given more than announced.
      5. Equal rates: a pass-through, no latency, the process gets the host's
         samples as they are.
      6. reset() is bit-exact.
      7. Cost: the round trip's CPU per host rate, reported.

    Exit code 0 when everything passes.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/dsp/Resampler.h>

#include <chrono>
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
    constexpr double pi = 3.14159265358979323846;
    const double hostRates[] { 44100.0, 88200.0, 96000.0, 176400.0, 192000.0 };

    std::vector<float> sine (double hz, double rate, int n, double amp = 0.5)
    {
        std::vector<float> x ((size_t) n);
        for (int i = 0; i < n; ++i)
            x[(size_t) i] = (float) (amp * std::sin (2.0 * pi * hz * i / rate));
        return x;
    }

    /** Through a container around an identity process, in blocks of `block`
        (0: random sizes up to `maxBlock`). */
    std::vector<float> roundTrip (fxme::ResamplingContainer& c, std::vector<float> x, int maxBlock, int block,
                                  int* worstInner = nullptr)
    {
        unsigned rng = 7;
        int worst = 0;
        for (size_t start = 0; start < x.size();)
        {
            int n = block;
            if (block == 0)
            {
                rng = rng * 1664525u + 1013904223u;
                n = 1 + (int) ((rng >> 8) % (unsigned) maxBlock);
            }
            n = (int) std::min<size_t> ((size_t) n, x.size() - start);
            c.process (x.data() + start, n, [&worst] (const float* in, float* out, int m)
            {
                worst = std::max (worst, m);
                std::copy (in, in + m, out);
            });
            start += (size_t) n;
        }
        if (worstInner != nullptr)
            *worstInner = worst;
        return x;
    }

    /** Error of y against x delayed by `delay`, in dB under x, over the second half. */
    double errorDb (const std::vector<float>& x, const std::vector<float>& y, int delay)
    {
        double e = 0.0, s = 0.0;
        for (size_t i = y.size() / 2; i < y.size(); ++i)
        {
            const double ref = (double) x[i - (size_t) delay];
            e += (y[i] - ref) * (y[i] - ref);
            s += ref * ref;
        }
        return 10.0 * std::log10 (e / s + 1.0e-30);
    }

    double rms (const std::vector<float>& x, size_t from)
    {
        double s = 0.0;
        for (size_t i = from; i < x.size(); ++i)
            s += (double) x[i] * x[i];
        return std::sqrt (s / (double) (x.size() - from));
    }

    /** Amplitude of `hz` in y over its second half (one DFT bin). */
    double amplitude (const std::vector<float>& y, double hz, double rate)
    {
        double re = 0.0, im = 0.0;
        const size_t from = y.size() / 2;
        for (size_t i = from; i < y.size(); ++i)
        {
            re += y[i] * std::cos (2.0 * pi * hz * (double) i / rate);
            im -= y[i] * std::sin (2.0 * pi * hz * (double) i / rate);
        }
        return 2.0 * std::sqrt (re * re + im * im) / (double) (y.size() - from);
    }
}

int main()
{
    std::printf ("1. Exact latency\n");
    {
        bool ok = true;
        for (double rate : hostRates)
        {
            fxme::ResamplingContainer c;
            c.prepare (rate, 48000.0, 512);
            const int d = c.getLatencySamples();
            double worst = -1000.0, offByOne = 0.0;
            for (double hz : { 1000.0, 10000.0 })
            {
                c.reset();
                const auto x = sine (hz, rate, (int) rate);
                const auto y = roundTrip (c, x, 512, 256);
                worst = std::max (worst, errorDb (x, y, d));
                offByOne = std::max (offByOne, errorDb (x, y, d + 1));
            }
            std::printf ("     %6.0f Hz host: latency %d samples, error %.1f dB (one off: %.1f dB)\n", rate, d, worst, offByOne);
            ok = ok && worst < -80.0 && offByOne > -40.0;
        }
        check (ok, "output = input delayed by exactly the latency, every rate");
    }

    std::printf ("2. Flat passband\n");
    {
        bool ok = true;
        for (double rate : { 44100.0, 96000.0 })
        {
            fxme::ResamplingContainer c;
            c.prepare (rate, 48000.0, 512);
            const auto low = roundTrip (c, sine (1000.0, rate, (int) rate), 512, 256);
            c.reset();
            const auto high = roundTrip (c, sine (20000.0, rate, (int) rate), 512, 256);
            const double db = 20.0 * std::log10 (amplitude (high, 20000.0, rate) / amplitude (low, 1000.0, rate));
            std::printf ("     %6.0f Hz host: 20 kHz against 1 kHz: %+.3f dB\n", rate, db);
            ok = ok && std::abs (db) < 0.1;
        }
        check (ok, "within 0.1 dB at 20 kHz (round trip)");
    }

    std::printf ("3. Aliasing\n");
    {
        auto folded = [] (double inRate, double outRate, double hz)
        {
            fxme::Resampler r;
            r.prepare (inRate, outRate, 512);
            const auto x = sine (hz, inRate, (int) inRate);
            std::vector<float> y;
            std::vector<float> buf ((size_t) r.maxOutputFor (512));
            for (size_t i = 0; i < x.size(); i += 512)
            {
                const int n = (int) std::min<size_t> (512, x.size() - i);
                const int p = r.process (x.data() + i, n, buf.data());
                y.insert (y.end(), buf.begin(), buf.begin() + p);
            }
            return 20.0 * std::log10 (rms (y, y.size() / 2) / rms (x, 0) + 1.0e-12);
        };
        const double a = folded (96000.0, 48000.0, 30000.0), b = folded (48000.0, 44100.0, 23000.0);
        std::printf ("     96 -> 48 kHz, 30 kHz tone: %.1f dB; 48 -> 44.1 kHz, 23 kHz tone: %.1f dB\n", a, b);
        check (a < -90.0 && b < -90.0, "under -90 dB");
    }

    std::printf ("4. Any block sizes\n");
    {
        bool ok = true;
        for (double rate : hostRates)
        {
            fxme::ResamplingContainer c;
            c.prepare (rate, 48000.0, 512);
            int worstInner = 0;
            const auto x = sine (3000.0, rate, (int) (2 * rate));
            const auto y = roundTrip (c, x, 512, 0, &worstInner);
            const double err = errorDb (x, y, c.getLatencySamples());
            ok = ok && c.getUnderruns() == 0 && worstInner <= c.getMaxInnerBlock() && err < -80.0;
            if (c.getUnderruns() != 0 || err >= -80.0)
                std::printf ("     %6.0f Hz host: %lld underruns, error %.1f dB\n", rate, (long long) c.getUnderruns(), err);
        }
        check (ok, "random blocks 1..512: no underrun, inner blocks within the maximum, exact output");
    }

    std::printf ("5. Equal rates\n");
    {
        fxme::ResamplingContainer c;
        c.prepare (48000.0, 48000.0, 256);
        const auto x = sine (5000.0, 48000.0, 4800);
        const auto y = roundTrip (c, x, 256, 128);
        check (c.isBypassed() && c.getLatencySamples() == 0 && y == x, "a pass-through, no latency");
    }

    std::printf ("6. Reset\n");
    {
        fxme::ResamplingContainer c;
        c.prepare (44100.0, 48000.0, 512);
        const auto x = sine (440.0, 44100.0, 20000);
        const auto first = roundTrip (c, x, 512, 0);
        c.reset();
        check (roundTrip (c, x, 512, 0) == first, "bit-exact after reset()");
    }

    std::printf ("7. Cost (round trip, one mono instance, one core)\n");
    for (double rate : hostRates)
    {
        fxme::ResamplingContainer c;
        c.prepare (rate, 48000.0, 256);
        const auto x = sine (1000.0, rate, (int) (5 * rate));
        const auto t0 = std::chrono::steady_clock::now();
        roundTrip (c, x, 256, 256);
        const double t = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        std::printf ("     %6.0f Hz host: %.2f %% of a core\n", rate, 100.0 * t / 5.0);
    }

    std::printf (failures == 0 ? "All resampler tests passed\n" : "%d resampler test(s) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
