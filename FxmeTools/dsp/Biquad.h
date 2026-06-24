/*
  ------------------------------------------------------------------------------
    Biquad.h

    Minimal allocation-free biquad (RBJ cookbook). juce::dsp::IIR coefficients
    are reference-counted (allocate), which we must avoid when settings are
    applied on the audio thread. Header-only, depends only on <cmath>.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <cmath>

namespace fxme
{

struct BiquadCoeffs
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;

    static BiquadCoeffs lowpass (double sr, float freq, float q)
    {
        return fromRbj (sr, freq, q, Shape::lp);
    }
    static BiquadCoeffs highpass (double sr, float freq, float q)
    {
        return fromRbj (sr, freq, q, Shape::hp);
    }
    static BiquadCoeffs bandpass (double sr, float freq, float q)
    {
        return fromRbj (sr, freq, q, Shape::bp);
    }

    /** Peaking ("Band") EQ: boosts/cuts gainDb around freq with bandwidth set
        by q. (RBJ cookbook peakingEQ.) */
    static BiquadCoeffs peaking (double sr, float freq, float q, float gainDb)
    {
        BiquadCoeffs c;
        const double f = std::fmin ((double) freq, 0.49 * sr);
        const double A = std::pow (10.0, (double) gainDb / 40.0);
        const double w0 = 2.0 * 3.14159265358979323846 * f / sr;
        const double cw = std::cos (w0), sw = std::sin (w0);
        const double alpha = sw / (2.0 * std::fmax (0.01, (double) q));
        const double a0 = 1.0 + alpha / A;

        c.b0 = (float) ((1.0 + alpha * A) / a0);
        c.b1 = (float) ((-2.0 * cw) / a0);
        c.b2 = (float) ((1.0 - alpha * A) / a0);
        c.a1 = (float) ((-2.0 * cw) / a0);
        c.a2 = (float) ((1.0 - alpha / A) / a0);
        return c;
    }

    /** Low-shelf: boosts/cuts gainDb below freq (RBJ cookbook lowShelf, slope
        S = 1, so there is no Q parameter — matches the legacy EQ behaviour). */
    static BiquadCoeffs lowShelf (double sr, float freq, float gainDb)
    {
        return shelf (sr, freq, gainDb, true);
    }

    /** High-shelf: boosts/cuts gainDb above freq (RBJ cookbook highShelf,
        slope S = 1). */
    static BiquadCoeffs highShelf (double sr, float freq, float gainDb)
    {
        return shelf (sr, freq, gainDb, false);
    }

private:
    enum class Shape { lp, hp, bp };

    /** Shared RBJ shelving filter (slope S = 1). `low` selects low vs high. */
    static BiquadCoeffs shelf (double sr, float freq, float gainDb, bool low)
    {
        BiquadCoeffs c;
        const double f = std::fmin ((double) freq, 0.49 * sr);
        const double A = std::pow (10.0, (double) gainDb / 40.0);
        const double w0 = 2.0 * 3.14159265358979323846 * f / sr;
        const double cw = std::cos (w0), sw = std::sin (w0);
        // S = 1  ->  alpha = sin(w0)/2 * sqrt(2).
        const double alpha = sw * 0.5 * std::sqrt (2.0);
        const double tsa = 2.0 * std::sqrt (A) * alpha;

        double b0, b1, b2, a0, a1, a2;
        if (low)
        {
            b0 =        A * ((A + 1.0) - (A - 1.0) * cw + tsa);
            b1 =  2.0 * A * ((A - 1.0) - (A + 1.0) * cw);
            b2 =        A * ((A + 1.0) - (A - 1.0) * cw - tsa);
            a0 =            (A + 1.0) + (A - 1.0) * cw + tsa;
            a1 = -2.0 *     ((A - 1.0) + (A + 1.0) * cw);
            a2 =            (A + 1.0) + (A - 1.0) * cw - tsa;
        }
        else
        {
            b0 =        A * ((A + 1.0) + (A - 1.0) * cw + tsa);
            b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw);
            b2 =        A * ((A + 1.0) + (A - 1.0) * cw - tsa);
            a0 =            (A + 1.0) - (A - 1.0) * cw + tsa;
            a1 =  2.0 *     ((A - 1.0) - (A + 1.0) * cw);
            a2 =            (A + 1.0) - (A - 1.0) * cw - tsa;
        }

        c.b0 = (float) (b0 / a0);
        c.b1 = (float) (b1 / a0);
        c.b2 = (float) (b2 / a0);
        c.a1 = (float) (a1 / a0);
        c.a2 = (float) (a2 / a0);
        return c;
    }

    static BiquadCoeffs fromRbj (double sr, float freq, float q, Shape shape)
    {
        BiquadCoeffs c;
        const double f = std::fmin ((double) freq, 0.49 * sr);
        const double w0 = 2.0 * 3.14159265358979323846 * f / sr;
        const double cw = std::cos (w0), sw = std::sin (w0);
        const double alpha = sw / (2.0 * std::fmax (0.01, (double) q));
        const double a0 = 1.0 + alpha;
        double b0 = 1.0, b1 = 0.0, b2 = 0.0;

        switch (shape)
        {
            case Shape::lp: b1 = 1.0 - cw; b0 = b2 = b1 * 0.5; break;
            case Shape::hp: b1 = -(1.0 + cw); b0 = b2 = -b1 * 0.5; break;
            case Shape::bp: b0 = alpha; b1 = 0.0; b2 = -alpha; break;
        }

        c.b0 = (float) (b0 / a0);
        c.b1 = (float) (b1 / a0);
        c.b2 = (float) (b2 / a0);
        c.a1 = (float) (-2.0 * cw / a0);
        c.a2 = (float) ((1.0 - alpha) / a0);
        return c;
    }
};

struct Biquad
{
    BiquadCoeffs c;
    float z1 = 0.0f, z2 = 0.0f;

    void reset() noexcept { z1 = z2 = 0.0f; }

    // Transposed direct form II
    inline float processSample (float x) noexcept
    {
        const float y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return y;
    }

    void processBlock (float* data, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
            data[i] = processSample (data[i]);
    }
};

} // namespace fxme
