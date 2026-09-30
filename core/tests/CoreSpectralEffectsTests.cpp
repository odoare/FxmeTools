/*
  ------------------------------------------------------------------------------
    CoreSpectralEffectsTests.cpp

    fxme::SpectralBandEffects inside a fxme::SpectralBandSplitter, on tones:

      1. Installed with every effect off, it changes nothing, sample for
         sample, even with the analysis kept running.
      2. Pitch lands a tone where the ratio says, for non-integer intervals
         too, up and down, at about the same level.
      3. A tonal freeze holds the tone at its pitch and level after the input
         stops, and ignores a new input.
      4. A wash freeze holds the level, and is reproducible exactly.
      5. Blur leaves a steady tone alone, and keeps a stopped tone sounding.
      6. Freeze width, in stereo: none leaves identical channels identical;
         full width decorrelates them (a tonal freeze keeping its pitch and
         level on each side, a wash more so), reproducibly.
      7. A retriggered freeze captures the new input without a gap.

    Frequencies are measured from interpolated upward zero crossings over a
    long window, which is exact enough for a single dominant partial.

    Exit code 0 when everything passes.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/dsp/SpectralBandEffects.h>

#include <algorithm>
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

    /** run() in stereo, the same input on both sides: left then right. */
    std::vector<float> runStereo (const Signal& input, const Schedule& schedule,
                                  double seconds, std::vector<float>& right,
                                  const std::function<void (fxme::SpectralBandEffects&, int)>& atBlock = {})
    {
        fxme::SpectralBandSplitter splitter;
        fxme::SpectralBandEffects effects;

        splitter.prepare (sampleRate, blockSize, 1, order, 2);
        splitter.setApplyPan (false);
        effects.prepare (splitter.getFftSize(), splitter.getHopSize(), sampleRate, 1);
        splitter.setBandProcessor (&effects);

        fxme::SpectralBand band;
        band.enabled = true;
        band.lowHz = 20.0f;
        band.highHz = 20000.0f;
        splitter.setBand (0, band);

        const int total = (int) (seconds * sampleRate);
        std::vector<float> in ((size_t) blockSize), left;
        right.clear();

        for (int start = 0; start < total; start += blockSize)
        {
            effects.setBand (0, schedule (start));
            if (atBlock)
                atBlock (effects, start);
            for (int i = 0; i < blockSize; ++i)
                in[(size_t) i] = input (start + i);

            splitter.process (in.data(), in.data(), blockSize);
            const float* l = splitter.getBandOutput (0, 0);
            const float* r = splitter.getBandOutput (0, 1);
            left.insert (left.end(), l, l + blockSize);
            right.insert (right.end(), r, r + blockSize);
        }

        return left;
    }

    /** Normalised correlation of two signals over [fromSec, toSec). */
    double correlation (const std::vector<float>& a, const std::vector<float>& b,
                        double fromSec, double toSec)
    {
        const auto from = (size_t) (fromSec * sampleRate), to = (size_t) (toSec * sampleRate);
        double ab = 0.0, aa = 0.0, bb = 0.0;
        for (size_t i = from; i < to && i < a.size() && i < b.size(); ++i)
        {
            ab += (double) a[i] * b[i];
            aa += (double) a[i] * a[i];
            bb += (double) b[i] * b[i];
        }
        return aa > 0.0 && bb > 0.0 ? ab / std::sqrt (aa * bb) : 0.0;
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
    {
        Settings tracking;
        tracking.keepTracking = true;
        check (live == run (tone, constant (tracking), 1.0),
               "keeping the analysis running alone changes nothing either");
    }

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

    // ---- 6. freeze width --------------------------------------------------
    {
        const int freezeAt = (int) (0.4 * sampleRate);
        const int stopAt   = (int) (0.6 * sampleRate);

        // A chord of five partials between bins, so the correlation is an
        // average over several random spreads rather than one.
        const Signal chord = [=] (int n)
        {
            if (n >= stopAt)
                return 0.0f;
            float x = 0.0f;
            for (double hz : { 310.0, 587.0, 1033.0, 1571.0, 2219.0 })
                x += 0.1f * (float) std::sin (2.0 * pi * hz * n / sampleRate);
            return x;
        };

        const auto frozenWith = [=] (float width, fxme::SpectralBandEffects::FreezeMode mode)
        {
            Settings s;
            s.freezeOn = true;
            s.freezeMode = mode;
            s.freezeWidth = width;
            return Schedule ([=] (int n) { return n >= freezeAt ? s : Settings{}; });
        };

        using Mode = fxme::SpectralBandEffects::FreezeMode;
        std::vector<float> r0, r1, rw, rw2, rt;
        const auto l0 = runStereo (chord, frozenWith (0.0f, Mode::tonal), 1.6, r0);
        const auto l1 = runStereo (chord, frozenWith (1.0f, Mode::tonal), 1.6, r1);
        const auto lw = runStereo (chord, frozenWith (1.0f, Mode::wash),  1.6, rw);

        char what[160];
        std::snprintf (what, sizeof what, "no width: a frozen mono image stays mono (correlation %.3f)",
                       correlation (l0, r0, 1.0, 1.6));
        check (correlation (l0, r0, 1.0, 1.6) > 0.999, what);

        std::snprintf (what, sizeof what, "full width: a tonal freeze decorrelates the channels (%.3f)",
                       correlation (l1, r1, 1.0, 1.6));
        check (correlation (l1, r1, 1.0, 1.6) < 0.7, what);

        std::snprintf (what, sizeof what, "full width: each side keeps the tonal freeze's level (%.2f / %.2f dB)",
                       20.0 * std::log10 (rms (l1, 1.0, 1.6) / rms (l0, 1.0, 1.6)),
                       20.0 * std::log10 (rms (r1, 1.0, 1.6) / rms (r0, 1.0, 1.6)));
        check (levelClose (rms (l1, 1.0, 1.6), rms (l0, 1.0, 1.6), 1.0)
                   && levelClose (rms (r1, 1.0, 1.6), rms (r0, 1.0, 1.6), 1.0), what);

        std::vector<float> rTone;
        const auto lTone = runStereo (sine (1000.0, stopAt), frozenWith (1.0f, Mode::tonal), 1.6, rTone);
        check (within (frequency (lTone, 1.0, 1.6), 1000.0, 0.005)
                   && within (frequency (rTone, 1.0, 1.6), 1000.0, 0.005),
               "full width: a tonal freeze holds the pitch on both sides");

        std::snprintf (what, sizeof what, "full width: a wash decorrelates the channels (%.3f)",
                       correlation (lw, rw, 1.0, 1.6));
        check (std::abs (correlation (lw, rw, 1.0, 1.6)) < 0.3, what);

        const auto lw2 = runStereo (chord, frozenWith (1.0f, Mode::wash), 1.6, rw2);
        check (lw == lw2 && rw == rw2, "a wide wash is reproducible sample for sample");
    }

    // ---- 7. retrigger --------------------------------------------------------
    {
        // Frozen from 0.4 s on a 1500 Hz tone; the input moves to 1000 Hz at
        // 0.6 s and the freeze is retriggered at 0.8 s: afterwards it holds
        // 1000 Hz, at the level, and was never silent in between.
        const int freezeAt  = (int) (0.4 * sampleRate);
        const int changeAt  = (int) (0.6 * sampleRate);
        const int retrigAt  = (int) (0.8 * sampleRate);

        Settings frozen;
        frozen.freezeOn = true;
        const Schedule schedule = [=] (int n) { return n >= freezeAt ? frozen : Settings{}; };

        std::vector<float> right;
        const auto out = runStereo (sine (toneHz, changeAt, 1000.0), schedule, 1.6, right,
                                    [=] (fxme::SpectralBandEffects& fx, int start)
                                    {
                                        if (start <= retrigAt && retrigAt < start + blockSize)
                                            fx.retriggerFreeze (0);
                                    });

        check (within (frequency (out, 0.65, 0.8), toneHz, 0.005),
               "before the retrigger, the freeze holds the first capture");
        check (within (frequency (out, 1.0, 1.6), 1000.0, 0.005),
               "after the retrigger, it holds the new input");
        check (levelClose (rms (out, 1.0, 1.6), liveRms, 3.0),
               "after the retrigger, the level holds");

        char what[160];
        double quietest = 1.0;
        for (double t = 0.75; t < 1.0; t += 0.005)
            quietest = std::min (quietest, rms (out, t, t + 0.005));
        std::snprintf (what, sizeof what, "no gap around the retrigger (quietest 5 ms: %.1f dB)",
                       20.0 * std::log10 (quietest / liveRms));
        check (quietest > 0.25 * liveRms, what);
    }

    std::printf ("\n%s (%d failures)\n",
                 failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
