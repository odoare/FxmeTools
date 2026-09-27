/*
  ------------------------------------------------------------------------------
    CoreSpectralEffectsTests.cpp

    fxme::SpectralBandEffects inside a fxme::SpectralBandSplitter, on tones:

      1. Installed with every effect off, it changes nothing, sample for
         sample.
      2. Pitch lands a tone where the ratio says, for non-integer intervals
         too, up and down, at about the same level.
      3. A tonal freeze holds the tone at its pitch and level after the input
         stops, and ignores a new input.
      4. A wash freeze holds the level, and is reproducible exactly.
      5. Blur leaves a steady tone alone, and keeps a stopped tone sounding.

    Frequencies are measured from interpolated upward zero crossings over a
    long window, which is exact enough for a single dominant partial.

    Exit code 0 when everything passes.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/dsp/SpectralBandEffects.h>

#include <cmath>
#include <cstdio>
#include <functional>
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
    using Settings = fxme::SpectralBandEffects::Settings;

    constexpr double sampleRate = 48000.0;
    constexpr int    order      = 11;
    constexpr int    blockSize  = 512;
    constexpr double pi         = 3.141592653589793238;

    /** Input sample at index n. */
    using Signal   = std::function<float (int n)>;
    /** Effect settings for the block starting at index n. */
    using Schedule = std::function<Settings (int n)>;

    Signal sine (double hz, int stopAt = -1, double hzAfter = 0.0)
    {
        return [=] (int n)
        {
            if (stopAt >= 0 && n >= stopAt)
                return hzAfter > 0.0 ? 0.5f * (float) std::sin (2.0 * pi * hzAfter * n / sampleRate)
                                     : 0.0f;
            return 0.5f * (float) std::sin (2.0 * pi * hz * n / sampleRate);
        };
    }

    /** Runs `seconds` of `input` through a one-band splitter (pan off, band
        wide open) with the effects installed (or not), and returns the
        output. */
    std::vector<float> run (const Signal& input, const Schedule& schedule,
                            double seconds, bool install = true)
    {
        fxme::SpectralBandSplitter splitter;
        fxme::SpectralBandEffects effects;

        splitter.prepare (sampleRate, blockSize, 1, order);
        splitter.setApplyPan (false);
        effects.prepare (splitter.getFftSize(), splitter.getHopSize(), sampleRate, 1);
        if (install)
            splitter.setBandProcessor (&effects);

        fxme::SpectralBand band;
        band.enabled = true;
        band.lowHz = 20.0f;
        band.highHz = 20000.0f;
        splitter.setBand (0, band);

        const int total = (int) (seconds * sampleRate);
        std::vector<float> in ((size_t) blockSize), out;
        out.reserve ((size_t) total);

        for (int start = 0; start < total; start += blockSize)
        {
            effects.setBand (0, schedule (start));
            for (int i = 0; i < blockSize; ++i)
                in[(size_t) i] = input (start + i);

            splitter.process (in.data(), blockSize);
            const float* o = splitter.getBandOutput (0, 0);
            out.insert (out.end(), o, o + blockSize);
        }

        return out;
    }

    Schedule constant (Settings s)       { return [s] (int) { return s; }; }

    double rms (const std::vector<float>& x, double fromSec, double toSec)
    {
        const auto a = (size_t) (fromSec * sampleRate), b = (size_t) (toSec * sampleRate);
        double sum = 0.0;
        for (size_t i = a; i < b && i < x.size(); ++i)
            sum += (double) x[i] * x[i];
        return std::sqrt (sum / (double) (b - a));
    }

    /** Frequency from interpolated upward zero crossings in [fromSec, toSec). */
    double frequency (const std::vector<float>& x, double fromSec, double toSec)
    {
        const auto a = (size_t) (fromSec * sampleRate), b = (size_t) (toSec * sampleRate);
        double first = -1.0, last = -1.0;
        int crossings = 0;
        for (size_t i = a + 1; i < b && i < x.size(); ++i)
        {
            if (x[i - 1] < 0.0f && x[i] >= 0.0f)
            {
                const double t = (double) (i - 1) + x[i - 1] / (x[i - 1] - x[i]);
                if (first < 0.0) first = t;
                last = t;
                ++crossings;
            }
        }
        return crossings > 1 ? (crossings - 1) * sampleRate / (last - first) : 0.0;
    }

    bool within (double value, double target, double relative)
    {
        return std::abs (value - target) <= relative * std::abs (target);
    }

    bool levelClose (double a, double b, double dB)
    {
        return b > 0.0 && std::abs (20.0 * std::log10 (a / b)) <= dB;
    }
}

int main()
{
    std::printf ("SpectralBandEffects\n");

    constexpr double toneHz = 1500.0;    // bin 64 of the 2048-point window
    const auto tone = sine (toneHz);

    const auto live = run (tone, constant ({}), 1.0);
    const double liveRms = rms (live, 0.5, 1.0);

    // ---- 1. off is off -----------------------------------------------------
    check (live == run (tone, constant ({}), 1.0, false),
           "installed with every effect off, the output is unchanged sample for sample");

    // ---- 2. pitch ------------------------------------------------------------
    // 1500 Hz sits exactly on a bin; 1000 Hz falls between two (bin 42.7), so
    // it only lands right if the frequency tracking works.
    for (double hz : { toneHz, 1000.0 })
    {
        const auto in = sine (hz);
        const double inRms = rms (run (in, constant ({}), 1.0), 0.5, 1.0);

        for (float st : { 7.0f, -12.0f, 3.33f })
        {
            Settings s;
            s.pitchOn = true;
            s.pitchSemitones = st;
            const auto out = run (in, constant (s), 1.0);
            const double want = hz * std::exp2 (st / 12.0);
            const double got  = frequency (out, 0.4, 1.0);

            char what[160];
            std::snprintf (what, sizeof what, "pitch %+.2f st: %.1f Hz lands at %.1f Hz (want %.1f)",
                           st, hz, got, want);
            check (within (got, want, 0.005), what);

            std::snprintf (what, sizeof what, "pitch %+.2f st on %.0f Hz keeps the level within 1.5 dB (%.2f dB)",
                           st, hz, 20.0 * std::log10 (rms (out, 0.5, 1.0) / inRms));
            check (levelClose (rms (out, 0.5, 1.0), inRms, 1.5), what);
        }
    }

    // ---- 3. tonal freeze -----------------------------------------------------
    {
        // Freeze from 0.4 s; the input stops at 0.6 s (the capture is long
        // done), or changes pitch. The output is 2048 samples late.
        const int freezeAt = (int) (0.4 * sampleRate);
        const int stopAt   = (int) (0.6 * sampleRate);

        Settings frozen;
        frozen.freezeOn = true;
        const Schedule schedule = [=] (int n) { return n >= freezeAt ? frozen : Settings{}; };

        const auto held = run (sine (toneHz, stopAt), schedule, 1.6);
        check (levelClose (rms (held, 1.0, 1.6), liveRms, 3.0),
               "a tonal freeze holds the level after the input stops");
        check (within (frequency (held, 1.0, 1.6), toneHz, 0.005),
               "a tonal freeze holds the pitch after the input stops");

        const auto offBin = run (sine (1000.0, stopAt), schedule, 1.6);
        check (within (frequency (offBin, 1.0, 1.6), 1000.0, 0.005),
               "a tonal freeze holds the pitch of a tone between two bins");

        const auto ignored = run (sine (toneHz, stopAt, 3000.0), schedule, 1.6);
        check (within (frequency (ignored, 1.0, 1.6), toneHz, 0.005),
               "a frozen band ignores a new input");

        // ---- 4. wash freeze --------------------------------------------------
        Settings wash = frozen;
        wash.freezeMode = fxme::SpectralBandEffects::FreezeMode::wash;
        const Schedule washSchedule = [=] (int n) { return n >= freezeAt ? wash : Settings{}; };

        const auto washed = run (sine (toneHz, stopAt), washSchedule, 1.6);
        char what[160];
        std::snprintf (what, sizeof what, "a wash freeze holds the level within 3 dB (%.2f dB)",
                       20.0 * std::log10 (rms (washed, 1.0, 1.6) / liveRms));
        check (levelClose (rms (washed, 1.0, 1.6), liveRms, 3.0), what);
        check (washed == run (sine (toneHz, stopAt), washSchedule, 1.6),
               "a wash freeze is reproducible sample for sample");
    }

    // ---- 5. blur -------------------------------------------------------------
    {
        Settings blur;
        blur.blurOn = true;
        blur.blurSeconds = 0.5f;

        const auto steady = run (tone, constant (blur), 1.0);
        check (levelClose (rms (steady, 0.5, 1.0), liveRms, 1.0),
               "blur leaves a steady tone's level alone (within 1 dB)");
        check (within (frequency (steady, 0.5, 1.0), toneHz, 0.005),
               "blur leaves a steady tone's pitch alone");

        // Stop at 0.8 s; the output stops ~43 ms later without blur. Look
        // 100 to 150 ms after that.
        const int stopAt = (int) (0.8 * sampleRate);
        const double from = 0.8 + 2048.0 / sampleRate + 0.10, to = from + 0.05;

        const auto dry  = run (sine (toneHz, stopAt), constant ({}), 1.2);
        const auto tail = run (sine (toneHz, stopAt), constant (blur), 1.2);
        check (rms (dry, from, to) < 0.01 * liveRms, "without blur, a stopped tone is gone");
        check (rms (tail, from, to) > 0.3 * liveRms, "with a 500 ms blur, it is still sounding");
    }

    std::printf ("\n%s (%d failures)\n",
                 failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
