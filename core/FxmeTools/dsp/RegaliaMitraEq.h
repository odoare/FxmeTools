/*
  ------------------------------------------------------------------------------
    RegaliaMitraEq.h

    Parametric EQ sections whose gain is a scalar outside the recursive part,
    so it can be modulated on every sample (a dynamic EQ) without coefficient
    zipper, state discontinuities or clicks: the Regalia-Mitra structure.

    Bell. y = (1 + G)/2 x + (1 - G)/2 A(x), where A is a second-order allpass
    carrying the centre frequency and the bandwidth. At the centre A = -1 and
    the gain is G; far from it A = +1 and the gain is 1. The allpass is a
    two-stage lattice (Gray-Markel), whose reflection coefficients are
    decoupled: k1 = -cos (w0) holds the frequency, k2 = -c the bandwidth,
    with c = (tan (wb / 2) - V) / (tan (wb / 2) + V), wb = w0 / Q. The lattice
    stays well behaved while its coefficients move, which the bandwidth
    coefficient must do on every sample, because of:

    The cut/boost correction (Zolzer). With V = 1 a cut of -x dB is narrower
    than a boost of +x dB. Taking V = G for cuts (and 1 for boosts) makes a
    cut the exact inverse of the boost of the same size: the two responses
    sum to 0 dB at every frequency. V = min (G, 1) is continuous at G = 1,
    so a band whose gain crosses 0 dB (a static boost with a dynamic cut, say)
    moves smoothly from one regime to the other; V is additionally passed
    through a short one-pole (setCorrectionSmoothing, 0.5 ms by default), so a
    jump in G (a preset load) turns into a fast glide of the bandwidth rather
    than a step. G itself is applied as given: smoothing it is the caller's
    business (a dynamics processor's ballistics).

    Shelves. The same principle, y = x + (G - 1) S(x): the gain stays outside,
    S is a second-order low-pass (low shelf) or high-pass (high shelf) from a
    trapezoidal SVF (TptSvf.h), whose Q is the shelf's slope / resonance
    (0.7071: no overshoot). In a boost S's cutoff is the shelf frequency; in a
    cut the cutoff and Q are moved (cutoff / sqrt V for a low shelf, cutoff *
    sqrt V for a high shelf, Q / sqrt V) so that, as for the bell, a cut is
    the exact inverse of the boost of the same size. Those moved coefficients
    use the same smoothed V as the bell, and the SVF tolerates per-sample
    coefficient changes.

    Frequency and Q changes (from the user or automation) glide: a linear ramp
    of their logarithms over 20 ms (prepare's `smoothingSeconds`).

    Channels share the coefficients. Per sample: advance (G) once, then
    process (channel, x) for each channel that the band acts on.

    magnitude() gives the exact response of a section for drawing, from the
    same formulas, without running it.

    Header-only, no allocation, realtime safe.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/dsp/TptSvf.h>
#include <FxmeTools/util/Math.h>
#include <array>
#include <cmath>
#include <complex>

namespace fxme
{

class RegaliaMitraEq
{
public:
    enum class Shape { bell, lowShelf, highShelf };

    static constexpr int maxChannels = 2;

    //==========================================================================
    void prepare (double newSampleRate, float smoothingSeconds = 0.02f) noexcept
    {
        sampleRate  = newSampleRate > 0.0 ? newSampleRate : 48000.0;
        smoothSteps = fxme::jmax (1, (int) std::floor (smoothingSeconds * sampleRate));
        setCorrectionSmoothing (correctionSeconds);
        logFreq = logFreqTarget;
        logQ = logQTarget;
        stepsLeft = 0;
        updateGeometry();
        reset();
    }

    /** Clears the filter state; the gain correction snaps to G = 1. */
    void reset() noexcept
    {
        for (auto& s : lattice)
            s = {};
        for (auto& s : svf)
            s.reset();
        vSmoothed = 1.0;
        updateCorrection (1.0);
    }

    /** Time constant of the one-pole on the cut correction V (see the file
        comment). 0 disables it. */
    void setCorrectionSmoothing (float seconds) noexcept
    {
        correctionSeconds = fxme::jmax (0.0f, seconds);
        vCoef = correctionSeconds <= 0.0f
                    ? 0.0
                    : std::exp (-1.0 / ((double) correctionSeconds * sampleRate));
    }

    void setShape (Shape s) noexcept
    {
        if (s == shape)
            return;
        shape = s;
        reset();
        updateGeometry();
    }

    Shape getShape() const noexcept { return shape; }

    /** Frequency and Q to glide to over the smoothing time (or to jump to,
        with `snap`). Q is the bell's bandwidth (w0 / Q), the shelves' SVF Q. */
    void setFrequencyAndQ (float hz, float q, bool snap = false) noexcept
    {
        const double fT = std::log (fxme::jlimit (10.0, 0.49 * sampleRate, (double) hz));
        const double qT = std::log (fxme::jlimit (0.025, 40.0, (double) q));

        if (snap)
        {
            logFreq = logFreqTarget = fT;
            logQ = logQTarget = qT;
            stepsLeft = 0;
            updateGeometry();
            return;
        }

        if (fT == logFreqTarget && qT == logQTarget)
            return;

        logFreqTarget = fT;
        logQTarget = qT;
        stepsLeft = smoothSteps;
        logFreqStep = (logFreqTarget - logFreq) / stepsLeft;
        logQStep = (logQTarget - logQ) / stepsLeft;
    }

    /** Moves one sample on: the frequency / Q glide, and the gain `gain`
        (linear, > 0) that process() applies on this sample. Call once per
        sample, before process() for each channel. */
    void advance (float gain) noexcept
    {
        bool geometryChanged = false;
        if (stepsLeft > 0)
        {
            if (--stepsLeft == 0)
            {
                logFreq = logFreqTarget;
                logQ = logQTarget;
            }
            else
            {
                logFreq += logFreqStep;
                logQ += logQStep;
            }
            updateGeometry (false);
            geometryChanged = true;
        }

        g = fxme::jmax (1.0e-6, (double) gain);
        const double vTarget = g < 1.0 ? g : 1.0;
        const double vNew = vTarget + vCoef * (vSmoothed - vTarget);

        if (geometryChanged || vNew != vSmoothed)
        {
            vSmoothed = vNew;
            updateCorrection (vSmoothed);
        }

        halfSum = 0.5 * (1.0 + g);
        halfDiff = 0.5 * (1.0 - g);
    }

    float process (int channel, float x) noexcept
    {
        if (shape == Shape::bell)
        {
            // Two-stage lattice allpass: outer stage k2, inner stage k1.
            auto& s = lattice[(size_t) channel];
            const double e  = (double) x - k2 * s.u2;
            const double ap = k2 * e + s.u2;
            const double e1 = e - k1 * s.u1;
            const double o1 = k1 * e1 + s.u1;
            s.u1 = e1;
            s.u2 = o1;
            return (float) (halfSum * x + halfDiff * ap);
        }

        auto& f = svf[(size_t) channel];
        const auto o = f.tick (x);
        const double shaped = shape == Shape::lowShelf ? o.low : o.high;
        return (float) ((double) x + (g - 1.0) * shaped);
    }

    float getFrequency() const noexcept { return (float) std::exp (logFreq); }
    float getQ() const noexcept         { return (float) std::exp (logQ); }

    //==========================================================================
    /** The steady-state response of a section with these settings at `hz`
        (linear magnitude). The exact digital response, for drawing. */
    static double magnitude (Shape shape, double fcHz, double q, double gain,
                             double hz, double sampleRate) noexcept
    {
        gain = fxme::jmax (1.0e-6, gain);
        const double v = gain < 1.0 ? gain : 1.0;

        if (shape == Shape::bell)
        {
            double k1, k2;
            bellCoefficients (fcHz, q, v, sampleRate, k1, k2);
            const double w = 2.0 * MathConstants<double>::pi
                               * fxme::jlimit (0.0, 0.5 * sampleRate, hz) / sampleRate;
            const std::complex<double> z1 = std::polar (1.0, -w);
            const std::complex<double> z2 = z1 * z1;
            // A(z) = (a2 + a1 z^-1 + z^-2) / (1 + a1 z^-1 + a2 z^-2),
            // a1 = k1 (1 + k2), a2 = k2.
            const double a1 = k1 * (1.0 + k2), a2 = k2;
            const auto a = (a2 + a1 * z1 + z2) / (1.0 + a1 * z1 + a2 * z2);
            return std::abs (0.5 * (1.0 + gain) + 0.5 * (1.0 - gain) * a);
        }

        double gc, kc;
        shelfCoefficients (shape, fcHz, q, v, sampleRate, gc, kc);
        const double w = TptSvf::normalisedFrequency (hz, sampleRate, gc);
        const auto s = shape == Shape::lowShelf ? TptSvf::lowPassResponse (w, kc)
                                                : TptSvf::highPassResponse (w, kc);
        return std::abs (1.0 + (gain - 1.0) * s);
    }

    static double magnitudeDb (Shape shape, double fcHz, double q, double gainDb,
                               double hz, double sampleRate) noexcept
    {
        const double m = magnitude (shape, fcHz, q, std::pow (10.0, gainDb / 20.0), hz, sampleRate);
        return 20.0 * std::log10 (fxme::jmax (1.0e-9, m));
    }

private:
    /** Lattice coefficients of the bell: k1 = -cos w0, k2 = -c with the cut
        correction V. */
    static void bellCoefficients (double fcHz, double q, double v, double sampleRate,
                                  double& k1, double& k2) noexcept
    {
        const double pi = MathConstants<double>::pi;
        const double f = fxme::jlimit (10.0, 0.49 * sampleRate, fcHz);
        const double w0 = 2.0 * pi * f / sampleRate;
        k1 = -std::cos (w0);
        k2 = -cFor (bandwidthTan (w0, q), v);
    }

    static double bandwidthTan (double w0, double q) noexcept
    {
        // The bandwidth wb = w0 / Q, kept under Nyquist so the tangent stays
        // finite (very low Q at high frequencies saturates rather than wraps).
        const double wb = fxme::jmin (w0 / fxme::jmax (0.01, q), 0.98 * MathConstants<double>::pi);
        return std::tan (0.5 * wb);
    }

    static double cFor (double t, double v) noexcept { return (t - v) / (t + v); }

    /** SVF g and k of a shelf with the cut correction V. */
    static void shelfCoefficients (Shape shape, double fcHz, double q, double v, double sampleRate,
                                   double& gOut, double& kOut) noexcept
    {
        const double g0 = TptSvf::gFor (fcHz, sampleRate);
        const double sv = std::sqrt (v);
        gOut = shape == Shape::lowShelf ? g0 / sv : g0 * sv;
        kOut = sv / fxme::jmax (0.05, q);
    }

    /** Frequency- and Q-dependent parts, recomputed while they glide. */
    void updateGeometry (bool alsoCorrection = true) noexcept
    {
        const double f = std::exp (logFreq), q = std::exp (logQ);
        const double w0 = 2.0 * MathConstants<double>::pi
                            * fxme::jlimit (10.0, 0.49 * sampleRate, f) / sampleRate;
        k1 = -std::cos (w0);
        tanHalfBw = bandwidthTan (w0, q);
        svfG = TptSvf::gFor (f, sampleRate);
        svfQ = fxme::jmax (0.05, q);
        if (alsoCorrection)
            updateCorrection (vSmoothed);
    }

    /** The parts that depend on V (the cut correction). */
    void updateCorrection (double v) noexcept
    {
        if (shape == Shape::bell)
        {
            k2 = -cFor (tanHalfBw, v);
            return;
        }

        const double sv = std::sqrt (v);
        const double gc = shape == Shape::lowShelf ? svfG / sv : svfG * sv;
        const double kc = sv / svfQ;
        for (auto& f : svf)
            f.setup (gc, kc);
    }

    struct LatticeState
    {
        double u1 = 0.0, u2 = 0.0;
    };

    double sampleRate = 48000.0;
    Shape shape = Shape::bell;

    double logFreq = std::log (1000.0), logFreqTarget = std::log (1000.0), logFreqStep = 0.0;
    double logQ = std::log (0.7071), logQTarget = std::log (0.7071), logQStep = 0.0;
    int stepsLeft = 0, smoothSteps = 960;

    float correctionSeconds = 0.0005f;
    double vCoef = 0.0, vSmoothed = 1.0;

    double g = 1.0, halfSum = 1.0, halfDiff = 0.0;
    double k1 = 0.0, k2 = 0.0, tanHalfBw = 1.0;
    double svfG = 0.1, svfQ = 0.7071;

    std::array<LatticeState, maxChannels> lattice {};
    std::array<TptSvf, maxChannels> svf {};
};

} // namespace fxme
