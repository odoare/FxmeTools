/*
  ------------------------------------------------------------------------------
    CoreDynamicEqTests.cpp

    fxme::RegaliaMitraEq, fxme::SvfPassCascade and the dynamics stages of
    DynamicGainComputer.h, the building blocks of a dynamic EQ:

      1. Responses: a bell is G at its centre and 1 far from it; boost and
         cut of the same size are exact mirrors (their dB responses sum to 0
         at every frequency) for the bell and both shelves, which is what the
         cut correction is for; a shelf reaches G at its far end. The running
         filters agree with magnitude() on steady sines.
      2. Transparency: G = 1 passes the signal untouched (the bell exactly).
      3. No click when the gain crosses 0 dB: a static boost with a dynamic
         cut, the gain ramping from +6 to -6 dB in 2 ms, a tone at the band's
         centre. The output's second difference (what a click is made of)
         stays within the bound a clean tone at the louder gain would give.
         The same with the shelves, and with the frequency jumping a decade.
      4. HP / LP cascades: -3 dB at the cutoff for 12, 24 and 48 dB/oct, and
         the running filter matches magnitude().
      5. The gain computer's four modes, its knee (continuous, monotonic),
         its range clamp and an infinite ratio.
      6. Ballistics: attack and release reach 1 - 1/e of a step after their
         time constants, at 44.1 and at 192 kHz alike.
      7. Detector: RMS of a full-scale sine reads -3.01 dBFS, peak of a
         constant 0.5 reads -6.02.

    Exit code 0 when everything passes.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/dsp/DynamicGainComputer.h>
#include <FxmeTools/dsp/RegaliaMitraEq.h>
#include <FxmeTools/dsp/TptSvf.h>

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
    constexpr double pi = 3.141592653589793238;

    double toGain (double db) { return std::pow (10.0, db / 20.0); }
    double toDb (double g)    { return 20.0 * std::log10 (std::max (g, 1.0e-12)); }

    using Shape = fxme::RegaliaMitraEq::Shape;

    /** Amplitude of a steady tone through the filter (after settling). */
    double steadyGainDb (Shape shape, double fc, double q, double gainDb, double hz, double sr)
    {
        fxme::RegaliaMitraEq eq;
        eq.setShape (shape);
        eq.prepare (sr);
        eq.setFrequencyAndQ ((float) fc, (float) q, true);
        const float g = (float) toGain (gainDb);
        const int settle = (int) (sr * 0.5), measure = (int) (sr * 0.2);
        double peak = 0.0;
        for (int n = 0; n < settle + measure; ++n)
        {
            eq.advance (g);
            const float y = eq.process (0, (float) std::sin (2.0 * pi * hz * n / sr));
            if (n >= settle)
                peak = std::max (peak, (double) std::abs (y));
        }
        return toDb (peak);
    }

    /** Largest |second difference| of the output while the gain ramps from
        `fromDb` to `toDbV` over `rampSeconds`, a tone at `hz`, and the bound
        a clean tone at the louder of the two gains has. */
    struct ClickResult { double worst, bound; };

    ClickResult rampClick (Shape shape, double fc, double hz, double fromDb, double toDbV,
                           double rampSeconds, double sr, double jumpFcTo = 0.0)
    {
        fxme::RegaliaMitraEq eq;
        eq.setShape (shape);
        eq.prepare (sr);
        eq.setFrequencyAndQ ((float) fc, 1.0f, true);

        const int settle = (int) (sr * 0.3), ramp = (int) (sr * rampSeconds);
        const int after = (int) (sr * 0.1);
        double y1 = 0.0, y2 = 0.0, worst = 0.0;

        for (int n = 0; n < settle + ramp + after; ++n)
        {
            double db = fromDb;
            if (n >= settle)
                db = n >= settle + ramp ? toDbV
                                        : fromDb + (toDbV - fromDb) * (n - settle) / (double) ramp;
            if (jumpFcTo > 0.0 && n == settle)
                eq.setFrequencyAndQ ((float) jumpFcTo, 1.0f);

            eq.advance ((float) toGain (db));
            const double y = eq.process (0, (float) std::sin (2.0 * pi * hz * n / sr));
            if (n >= settle - 4)
                worst = std::max (worst, std::abs (y - 2.0 * y1 + y2));
            y2 = y1;
            y1 = y;
        }

        // A tone of amplitude A has a second difference of amplitude
        // A (2 sin (w / 2))^2.
        const double w = 2.0 * pi * hz / sr;
        const double a = toGain (std::max (fromDb, toDbV));
        return { worst, a * std::pow (2.0 * std::sin (0.5 * w), 2.0) };
    }

    bool near (double a, double b, double tol) { return std::abs (a - b) <= tol; }
}

int main()
{
    std::printf ("Dynamic EQ building blocks\n");
    char what[256];
    const double sr = 48000.0;

    // ---- 1. responses ------------------------------------------------------------------
    {
        const double atCentre = fxme::RegaliaMitraEq::magnitudeDb (Shape::bell, 1000.0, 2.0, 9.0, 1000.0, sr);
        const double far      = fxme::RegaliaMitraEq::magnitudeDb (Shape::bell, 1000.0, 2.0, 9.0, 30.0, sr);
        std::snprintf (what, sizeof what, "bell +9 dB: %.3f dB at its centre, %.3f dB at 30 Hz", atCentre, far);
        check (near (atCentre, 9.0, 0.01) && std::abs (far) < 0.1, what);

        const double cutCentre = fxme::RegaliaMitraEq::magnitudeDb (Shape::bell, 1000.0, 2.0, -9.0, 1000.0, sr);
        std::snprintf (what, sizeof what, "bell -9 dB: %.3f dB at its centre", cutCentre);
        check (near (cutCentre, -9.0, 0.01), what);

        for (auto shape : { Shape::bell, Shape::lowShelf, Shape::highShelf })
        {
            for (double gainDb : { 3.0, 12.0, 24.0 })
            {
                double worst = 0.0;
                for (int i = 0; i <= 200; ++i)
                {
                    const double f = 20.0 * std::pow (1000.0, i / 200.0);
                    const double sum = fxme::RegaliaMitraEq::magnitudeDb (shape, 800.0, 0.9, gainDb, f, sr)
                                     + fxme::RegaliaMitraEq::magnitudeDb (shape, 800.0, 0.9, -gainDb, f, sr);
                    worst = std::max (worst, std::abs (sum));
                }
                std::snprintf (what, sizeof what, "%s +/-%.0f dB are mirrors (|boost + cut| <= %.4f dB)",
                               shape == Shape::bell ? "bell" : (shape == Shape::lowShelf ? "low shelf" : "high shelf"),
                               gainDb, worst);
                check (worst < 0.01, what);
            }
        }

        const double lsLow  = fxme::RegaliaMitraEq::magnitudeDb (Shape::lowShelf, 500.0, 0.7071, 8.0, 20.0, sr);
        const double lsHigh = fxme::RegaliaMitraEq::magnitudeDb (Shape::lowShelf, 500.0, 0.7071, 8.0, 15000.0, sr);
        const double hsHigh = fxme::RegaliaMitraEq::magnitudeDb (Shape::highShelf, 2000.0, 0.7071, -8.0, 20000.0, sr);
        std::snprintf (what, sizeof what, "low shelf +8: %.2f dB at 20 Hz, %.2f at 15 kHz; high shelf -8: %.2f at 20 kHz",
                       lsLow, lsHigh, hsHigh);
        check (near (lsLow, 8.0, 0.1) && std::abs (lsHigh) < 0.1 && near (hsHigh, -8.0, 0.3), what);

        struct Case { Shape s; double fc, q, g, f; };
        for (const auto& c : { Case { Shape::bell, 1000.0, 3.0, 6.0, 1000.0 },
                               Case { Shape::bell, 1000.0, 3.0, -6.0, 1300.0 },
                               Case { Shape::lowShelf, 300.0, 1.2, -10.0, 200.0 },
                               Case { Shape::highShelf, 4000.0, 0.7, 7.0, 5000.0 } })
        {
            const double run = steadyGainDb (c.s, c.fc, c.q, c.g, c.f, sr);
            const double ref = fxme::RegaliaMitraEq::magnitudeDb (c.s, c.fc, c.q, c.g, c.f, sr);
            std::snprintf (what, sizeof what, "running filter %.3f dB vs magnitude() %.3f dB", run, ref);
            check (near (run, ref, 0.05), what);
        }
    }

    // ---- 2. transparency ---------------------------------------------------------------
    {
        fxme::RegaliaMitraEq eq;
        eq.prepare (sr);
        eq.setFrequencyAndQ (700.0f, 2.0f, true);
        double worst = 0.0;
        for (int n = 0; n < 4800; ++n)
        {
            eq.advance (1.0f);
            const float x = (float) std::sin (0.37 * n) * 0.8f;
            worst = std::max (worst, (double) std::abs (eq.process (0, x) - x));
        }
        std::snprintf (what, sizeof what, "bell at G = 1 is transparent (max error %.2e)", worst);
        check (worst < 1.0e-6, what);
    }

    // ---- 3. no click across 0 dB --------------------------------------------------------
    {
        for (auto shape : { Shape::bell, Shape::lowShelf, Shape::highShelf })
        {
            const double fc = shape == Shape::bell ? 1000.0 : (shape == Shape::lowShelf ? 2000.0 : 500.0);
            const auto r = rampClick (shape, fc, 1000.0, 6.0, -6.0, 0.002, sr);
            std::snprintf (what, sizeof what,
                           "%s: +6 -> -6 dB in 2 ms, second difference %.4f (clean tone bound %.4f)",
                           shape == Shape::bell ? "bell" : (shape == Shape::lowShelf ? "low shelf" : "high shelf"),
                           r.worst, r.bound);
            check (r.worst < 1.5 * r.bound, what);
        }

        const auto slow = rampClick (Shape::bell, 1000.0, 1000.0, 3.0, -12.0, 0.05, sr);
        std::snprintf (what, sizeof what, "bell: +3 -> -12 dB in 50 ms, second difference %.4f (bound %.4f)",
                       slow.worst, slow.bound);
        check (slow.worst < 1.2 * slow.bound, what);

        const auto jump = rampClick (Shape::bell, 200.0, 1000.0, 6.0, -6.0, 0.002, sr, 2000.0);
        std::snprintf (what, sizeof what, "bell: frequency 200 Hz -> 2 kHz with the crossing, second difference %.4f (bound %.4f)",
                       jump.worst, jump.bound);
        check (jump.worst < 1.5 * jump.bound, what);
    }

    // ---- 4. HP / LP cascades ------------------------------------------------------------
    {
        using Type = fxme::SvfPassCascade::Type;
        for (int slope = 0; slope < 3; ++slope)
        {
            const int n = fxme::SvfPassCascade::sectionsForSlope (slope);
            const double atFc = toDb (fxme::SvfPassCascade::magnitude (Type::highPass, n, 1000.0, 1.0, 1000.0, sr));
            const double octaveBelow = toDb (fxme::SvfPassCascade::magnitude (Type::highPass, n, 1000.0, 1.0, 250.0, sr));
            std::snprintf (what, sizeof what, "HP %d dB/oct: %.2f dB at fc, %.1f dB two octaves under",
                           12 * n, atFc, octaveBelow);
            check (near (atFc, -3.01, 0.05) && near (octaveBelow, -24.0 * n, 1.5), what);

            fxme::SvfPassCascade lp;
            lp.setType (Type::lowPass);
            lp.setNumSections (n);
            lp.prepare (sr);
            lp.setFrequency (2000.0f, true);
            // Amplitude from the RMS (x sqrt 2): at 16 samples per cycle the
            // largest sample can miss the true peak by a tenth of a dB.
            double sumSq = 0.0;
            int count = 0;
            for (int i = 0; i < 48000; ++i)
            {
                lp.advance();
                const float y = lp.process (0, (float) std::sin (2.0 * pi * 3000.0 * i / sr));
                if (i >= 24000)
                {
                    sumSq += (double) y * y;
                    ++count;
                }
            }
            const double peak = std::sqrt (2.0 * sumSq / count);
            const double ref = toDb (fxme::SvfPassCascade::magnitude (Type::lowPass, n, 2000.0, 1.0, 3000.0, sr));
            std::snprintf (what, sizeof what, "LP %d dB/oct at 3 kHz: running %.3f dB vs magnitude() %.3f dB",
                           12 * n, toDb (peak), ref);
            check (near (toDb (peak), ref, 0.05), what);
        }
    }

    // ---- 5. gain computer ----------------------------------------------------------------
    {
        using M = fxme::DynamicsMode;
        fxme::GainComputer c;

        c.set (M::compress, -20.0f, 4.0f, 0.0f, 24.0f);
        std::snprintf (what, sizeof what, "compress 4:1 at -20: -8 dBFS -> %.2f dB, -30 -> %.2f",
                       c.gainDb (-8.0f), c.gainDb (-30.0f));
        check (near (c.gainDb (-8.0f), -9.0, 1e-4) && c.gainDb (-30.0f) == 0.0f, what);

        c.set (M::compress, -20.0f, fxme::GainComputer::infiniteRatio, 0.0f, 24.0f);
        std::snprintf (what, sizeof what, "compress inf:1: -10 dBFS -> %.2f dB", c.gainDb (-10.0f));
        check (near (c.gainDb (-10.0f), -10.0, 1e-4), what);

        c.set (M::compress, -20.0f, 4.0f, 0.0f, 6.0f);
        std::snprintf (what, sizeof what, "range 6 dB clamps the cut: 0 dBFS -> %.2f dB", c.gainDb (0.0f));
        check (near (c.gainDb (0.0f), -6.0, 1e-4), what);

        c.set (M::expand, -40.0f, 2.0f, 0.0f, 24.0f);
        std::snprintf (what, sizeof what, "expand 2:1 at -40: -50 dBFS -> %.2f dB, -30 -> %.2f",
                       c.gainDb (-50.0f), c.gainDb (-30.0f));
        check (near (c.gainDb (-50.0f), -10.0, 1e-4) && c.gainDb (-30.0f) == 0.0f, what);

        c.set (M::upwardCompress, -30.0f, 2.0f, 0.0f, 6.0f);
        std::snprintf (what, sizeof what, "upward compress 2:1: -36 dBFS -> %.2f dB, -80 -> %.2f (range 6)",
                       c.gainDb (-36.0f), c.gainDb (-80.0f));
        check (near (c.gainDb (-36.0f), 3.0, 1e-4) && near (c.gainDb (-80.0f), 6.0, 1e-4), what);

        c.set (M::upwardExpand, -30.0f, 1.5f, 0.0f, 24.0f);
        std::snprintf (what, sizeof what, "upward expand 1.5:1: -20 dBFS -> %.2f dB, -40 -> %.2f",
                       c.gainDb (-20.0f), c.gainDb (-40.0f));
        check (near (c.gainDb (-20.0f), 5.0, 1e-4) && c.gainDb (-40.0f) == 0.0f, what);

        for (auto m : { M::compress, M::expand, M::upwardCompress, M::upwardExpand })
        {
            c.set (m, -20.0f, 3.0f, 12.0f, 24.0f);
            bool continuous = true, monotonic = true;
            float prev = c.gainDb (-60.0f);
            for (float l = -60.0f; l <= 0.0f; l += 0.01f)
            {
                const float v = c.gainDb (l);
                continuous = continuous && std::abs (v - prev) < 0.05f;
                const bool rising = m == M::upwardExpand || m == M::expand;
                monotonic = monotonic && (rising ? v >= prev - 1e-5f : v <= prev + 1e-5f);
                prev = v;
            }
            std::snprintf (what, sizeof what, "mode %d with a 12 dB knee: continuous and monotonic", (int) m);
            check (continuous && monotonic, what);
        }
    }

    // ---- 6. ballistics -------------------------------------------------------------------
    {
        for (double rate : { 44100.0, 192000.0 })
        {
            fxme::GainBallistics b;
            b.prepare (rate);
            b.setTimes (0.010f, 0.100f);
            const int attackN = (int) std::lround (0.010 * rate);
            float v = 0.0f;
            for (int i = 0; i < attackN; ++i)
                v = b.process (-10.0f);
            const double attackFrac = v / -10.0;

            for (int i = 0; i < (int) rate; ++i)
                b.process (-10.0f);
            const int releaseN = (int) std::lround (0.100 * rate);
            for (int i = 0; i < releaseN; ++i)
                v = b.process (0.0f);
            const double releaseFrac = 1.0 - v / -10.0;

            std::snprintf (what, sizeof what, "%.1f kHz: attack reaches %.3f, release %.3f after their times (want 0.632)",
                           rate / 1000.0, attackFrac, releaseFrac);
            check (near (attackFrac, 0.632, 0.01) && near (releaseFrac, 0.632, 0.01), what);
        }
    }

    // ---- 7. detector ---------------------------------------------------------------------
    {
        fxme::LevelDetector d;
        d.prepare (sr);
        d.setMode (fxme::LevelDetector::Mode::rms);
        d.setRmsWindow (0.05f);
        float l = 0.0f;
        for (int i = 0; i < 96000; ++i)
            l = d.process ((float) std::sin (2.0 * pi * 1000.0 * i / sr));
        std::snprintf (what, sizeof what, "RMS of a full-scale sine: %.2f dBFS", l);
        check (near (l, -3.01, 0.15), what);

        d.setMode (fxme::LevelDetector::Mode::peak);
        std::snprintf (what, sizeof what, "peak of 0.5: %.2f dBFS", d.process (0.5f));
        check (near (d.process (0.5f), -6.02, 0.01), what);
    }

    std::printf (failures == 0 ? "All passed.\n" : "%d failure(s).\n", failures);
    return failures == 0 ? 0 : 1;
}
