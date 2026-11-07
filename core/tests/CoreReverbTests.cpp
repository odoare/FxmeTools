/*
  ------------------------------------------------------------------------------
    CoreReverbTests.cpp

    fxme::TankReverb and fxme::EarlyReflections:

      1. The impulse response is finite and decays.
      2. Calibration: with no damping and no bass multiplier, the decay time
         measured on the impulse response (T30 of its Schroeder curve)
         matches the RT60 setting within 15 %, at 1, 2.5 and 6 s, and at two
         sizes.
      3. rt60At(): the mid, high and low decay times follow the settings
         (RT60, RT60 x hfRatio, RT60 x bassMultiplier) within 15 %.
      4. Bad input (NaN, infinities, huge values) gives finite output, and
         the tank keeps working afterwards.
      5. Freeze holds: after 4 s frozen the energy is within 3 dB of where it
         was, and it decays again once released.
      6. Shimmer at full amount on a 20 s decay stays bounded (no runaway),
         with one voice and with two (an octave and a fifth) at full.
      7. reset() is bit-exact.
      8. Early reflections: the first tap arrives at its time, scaled by the
         time scale; the stairwell pattern is regular.

    Exit code 0 when everything passes.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/dsp/EarlyReflections.h>
#include <FxmeTools/dsp/TankReverb.h>
#include <FxmeTools/util/Random.h>

#include <cmath>
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
    constexpr double fs = 48000.0;

    fxme::TankReverb::Settings plainSettings (float rt60, float size)
    {
        fxme::TankReverb::Settings s;
        s.rt60 = rt60;
        s.size = size;
        s.hfRatio = 1.0f;
        s.bassMultiplier = 1.0f;
        s.modDepth = 0.0f;
        return s;
    }

    /** Impulse response (left + right) of a tank with these settings. */
    std::vector<float> impulseResponse (const fxme::TankReverb::Settings& s, double seconds)
    {
        fxme::TankReverb t;
        t.setSettings (s);
        t.prepare (fs);
        const int n = (int) (seconds * fs);
        std::vector<float> y ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            float l, r;
            const float x = i == 0 ? 1.0f : 0.0f;
            t.process (x, x, l, r);
            y[(size_t) i] = l + r;
        }
        return y;
    }

    /** T30 of the Schroeder backward-integrated decay curve, as an RT60. */
    double measureRt60 (const std::vector<float>& y)
    {
        std::vector<double> edc (y.size());
        double acc = 0.0;
        for (size_t i = y.size(); i-- > 0;)
        {
            acc += (double) y[i] * y[i];
            edc[i] = acc;
        }
        const double total = edc[0];
        double t5 = -1.0, t35 = -1.0;
        for (size_t i = 0; i < edc.size(); ++i)
        {
            const double db = 10.0 * std::log10 (edc[i] / total + 1.0e-30);
            if (t5 < 0.0 && db <= -5.0)  t5 = (double) i / fs;
            if (t35 < 0.0 && db <= -35.0) { t35 = (double) i / fs; break; }
        }
        return t35 > 0.0 ? 2.0 * (t35 - t5) : -1.0;
    }

    double energy (const std::vector<float>& y, size_t from, size_t to)
    {
        double e = 0.0;
        for (size_t i = from; i < to && i < y.size(); ++i)
            e += (double) y[i] * y[i];
        return e / (double) (to - from);
    }
}

int main()
{
    std::printf ("1. Impulse response\n");
    {
        const auto y = impulseResponse (plainSettings (2.0f, 1.0f), 4.0);
        bool finite = true;
        for (float v : y)
            finite = finite && std::isfinite (v);
        check (finite, "finite");
        check (energy (y, 0, 48000) > 100.0 * energy (y, 144000, 192000), "decays (over 20 dB in 3 s)");
    }

    std::printf ("2. Calibration\n");
    {
        bool ok = true;
        for (float size : { 1.0f, 2.0f })
            for (float rt : { 1.0f, 2.5f, 6.0f })
            {
                const double measured = measureRt60 (impulseResponse (plainSettings (rt, size), rt * 1.5 + 1.0));
                const double err = std::abs (measured / rt - 1.0);
                std::printf ("     size %.1f, RT60 %.1f s: measured %.2f s (%.0f %%)\n", size, rt, measured, 100.0 * err);
                ok = ok && err < 0.15;
            }
        check (ok, "T30 within 15 % of the setting");
    }

    std::printf ("3. rt60At\n");
    {
        auto s = plainSettings (3.0f, 1.0f);
        s.hfRatio = 0.3f;
        s.bassMultiplier = 1.8f;
        s.dampHz = 3000.0f;
        s.crossoverHz = 200.0f;
        const double mid = fxme::TankReverb::rt60At (s, 1000.0, fs);
        const double high = fxme::TankReverb::rt60At (s, 18000.0, fs);
        const double low = fxme::TankReverb::rt60At (s, 20.0, fs);
        std::printf ("     20 Hz %.2f s, 1 kHz %.2f s, 18 kHz %.2f s\n", low, mid, high);
        check (std::abs (mid / 3.0 - 1.0) < 0.15, "mid: RT60");
        check (std::abs (high / (3.0 * 0.3) - 1.0) < 0.15, "high: RT60 x hfRatio");
        check (std::abs (low / (3.0 * 1.8) - 1.0) < 0.15, "low: RT60 x bass multiplier");
        s.freeze = true;
        check (std::isinf (fxme::TankReverb::rt60At (s, 1000.0, fs)), "frozen: infinite");
    }

    std::printf ("4. Bad input\n");
    {
        fxme::TankReverb t;
        t.prepare (fs);
        const float bad[] { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                            -std::numeric_limits<float>::infinity(), 1.0e30f };
        bool finite = true;
        float l, r;
        for (float b : bad)
        {
            t.process (b, b, l, r);
            finite = finite && std::isfinite (l) && std::isfinite (r);
        }
        double e = 0.0;
        for (int i = 0; i < 48000; ++i)
        {
            t.process (i == 0 ? 0.5f : 0.0f, 0.0f, l, r);
            finite = finite && std::isfinite (l) && std::isfinite (r);
            e += (double) l * l;
        }
        check (finite, "finite during and after bad input");
        check (e > 1.0e-4, "still reverberating afterwards");
    }

    std::printf ("5. Freeze\n");
    {
        fxme::TankReverb t;
        auto s = plainSettings (2.0f, 1.0f);
        s.hfRatio = 0.4f;   // the losses freeze must undo
        s.modDepth = 0.5f;
        t.setSettings (s);
        t.prepare (fs);
        fxme::Random rng (3);
        float l, r;
        for (int i = 0; i < 48000; ++i)
        {
            const float x = 0.3f * (rng.nextFloat() * 2.0f - 1.0f);
            t.process (x, x, l, r);
        }
        s.freeze = true;
        t.setSettings (s);
        auto meanEnergy = [&t] (int n, bool feed, fxme::Random& noise)
        {
            double e = 0.0;
            float a, b;
            for (int i = 0; i < n; ++i)
            {
                const float x = feed ? 0.3f * (noise.nextFloat() * 2.0f - 1.0f) : 0.0f;
                t.process (x, x, a, b);
                e += (double) t.getEnergy();
            }
            return e / n;
        };
        const double start = meanEnergy (24000, true, rng);     // input ignored while frozen
        meanEnergy (4 * 48000 - 48000, false, rng);
        const double after = meanEnergy (24000, false, rng);
        const double changeDb = 10.0 * std::log10 (after / start);
        std::printf ("     energy after 4 s frozen: %+.2f dB\n", changeDb);
        check (std::abs (changeDb) < 3.0, "holds within 3 dB");
        s.freeze = false;
        t.setSettings (s);
        meanEnergy (3 * 48000, false, rng);
        check (meanEnergy (24000, false, rng) < after * 0.01, "released: decays again");
    }

    std::printf ("6. Shimmer\n");
    {
        auto runShimmer = [] (float second)
        {
            fxme::TankReverb t;
            auto s = plainSettings (20.0f, 2.0f);
            s.shimmer = 1.0f;
            s.shimmer2 = second;
            s.shimmerSemitones2 = 7.0f;
            s.hfRatio = 0.7f;
            t.setSettings (s);
            t.prepare (fs);
            fxme::Random rng (9);
            float l, r, peak = 0.0f;
            bool finite = true;
            double late = 0.0, early = 0.0;
            for (int i = 0; i < 20 * 48000; ++i)
            {
                const float x = i < 24000 ? 0.5f * (rng.nextFloat() * 2.0f - 1.0f) : 0.0f;
                t.process (x, x, l, r);
                finite = finite && std::isfinite (l);
                peak = std::max (peak, std::abs (l));
                if (i >= 48000 && i < 96000) early += (double) l * l;
                if (i >= 18 * 48000 && i < 19 * 48000) late += (double) l * l;
            }
            std::printf ("     %s: peak %.2f, energy 1-2 s vs 18-19 s: %+.1f dB\n",
                         second > 0.0f ? "two voices" : "one voice", peak, 10.0 * std::log10 (late / early));
            return finite && peak < 4.0f && late < early;
        };
        check (runShimmer (0.0f), "one voice: bounded and decaying");
        check (runShimmer (1.0f), "two voices: bounded and decaying");
    }

    std::printf ("7. Reset\n");
    {
        fxme::TankReverb t;
        auto s = plainSettings (2.0f, 1.0f);
        s.modDepth = 0.7f;
        t.setSettings (s);
        t.prepare (fs);
        auto run = [&t]
        {
            std::vector<float> y (20000);
            float l, r;
            for (size_t i = 0; i < y.size(); ++i)
            {
                t.process (i == 0 ? 1.0f : 0.0f, 0.0f, l, r);
                y[i] = l - r;
            }
            return y;
        };
        const auto first = run();
        t.reset();
        check (run() == first, "bit-exact after reset()");
    }

    std::printf ("8. Early reflections\n");
    {
        auto firstArrival = [] (fxme::EarlyReflections::Pattern p, float scale)
        {
            fxme::EarlyReflections er;
            er.prepare (fs);
            er.setPattern (p);
            er.setTimeScale (scale);
            float l, r;
            for (int i = 0; i < 20000; ++i)
            {
                er.process (i == 0 ? 1.0f : 0.0f, i == 0 ? 1.0f : 0.0f, l, r);
                if (std::abs (l) + std::abs (r) > 1.0e-3f)
                    return (double) i / fs * 1000.0;
            }
            return -1.0;
        };
        const double room1 = firstArrival (fxme::EarlyReflections::Pattern::room, 1.0f);
        const double room2 = firstArrival (fxme::EarlyReflections::Pattern::room, 2.0f);
        std::printf ("     room: first reflection at %.2f ms (scale 1), %.2f ms (scale 2)\n", room1, room2);
        check (std::abs (room1 - 4.3) < 0.2, "first tap at its time");
        check (std::abs (room2 - 8.6) < 0.3, "scaled with the time scale");

        const auto taps = fxme::EarlyReflections::getTaps (fxme::EarlyReflections::Pattern::stairwell);
        bool regular = true;
        for (size_t i = 1; i < taps.size(); ++i)
            regular = regular && std::abs ((taps[i].ms - taps[i - 1].ms) - 8.5f) < 1.0e-4f;
        check (regular, "stairwell: a regular flutter");
    }

    std::printf (failures == 0 ? "All reverb tests passed\n" : "%d reverb test(s) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
