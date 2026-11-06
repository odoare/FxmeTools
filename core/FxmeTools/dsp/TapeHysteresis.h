/*
  ------------------------------------------------------------------------------
    TapeHysteresis.h

    Magnetic hysteresis of recording tape: the Jiles-Atherton model of the
    magnetisation M driven by the field H of the record head, solved sample by
    sample. This is what makes tape saturation sound like tape rather than a
    waveshaper: the curve is soft and level dependent, and it has a memory
    (the loop), so the harmonics and the phase shift move with level and
    frequency.

    The model (Jiles and Atherton 1986; discretised as in Holters and Zolzer,
    DAFx 2016, and used for tape by Chowdhury, DAFx 2019):

        M_an = Ms L ((H + alpha M) / a),     L (x) = coth (x) - 1/x (Langevin)

        dM/dH = [ dm (1 - c) (M_an - M) / ((1 - c) d k - alpha (M_an - M))
                  + c (Ms / a) L'(Q) ] / (1 - alpha c (Ms / a) L'(Q))

    with d the sign of dH/dt and dm 1 when M_an - M has that sign, else 0.
    Units are normalised: Ms = 1 and a = 1/3, so the anhysteretic curve has a
    unit slope at the origin and saturates at +-1. The other three:

      c       reversibility, 0..1. The small-signal gain is close to
              c / (1 - alpha c): a low c (under-biased tape) is quiet and
              distorted at low level, its gain rising with level; c near 1
              (over-biased) is nearly linear until it saturates.
      k       coercivity, the loop's half width in H. Wider, more memory.
      alpha   inter-domain coupling: steepens the curve; keep it small
              (alpha c < 1).

    The solver: H's time derivative by the alpha transform (0.9, between
    backward Euler and the bilinear one, which rings at Nyquist), then one
    fourth-order Runge-Kutta step per sample with H and dH/dt interpolated at
    the midpoint. Run it oversampled (2x at least): the loop makes harmonics
    well above the input's band.

    Tape formulations and bias. paramsFor (type, bias) maps a tape type and a
    bias amount (0..1, 0.5 calibrated) to c, k and alpha: more bias raises c
    and narrows the loop (cleaner, and darker, which the caller models with a
    gentle high-frequency loss); less bias lowers c and widens it (grit, the
    crossover-like distortion of under-biased tape). Each type also has a
    headroom (how many dB above the reference level it saturates, against
    the modern type) and a hiss level. approxGain() is the gain at a level
    near the reference (an H amplitude of 0.1), from a fit of the model
    within +-0.3 dB over the range paramsFor() produces: dividing the output
    by it keeps the level steady while bias and type change.

    Safety: the input is clamped (+-100), and a state that goes bad anyway
    (NaN, or |M| past 2) is cleared.

    Header-only, <cmath> only, no allocation, realtime safe. One instance per
    channel.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/dsp/Sanitise.h>
#include <cmath>

namespace fxme
{

class TapeHysteresis
{
public:
    enum class TapeType { modern = 0, vintage, highOutput };

    struct Params
    {
        float c = 0.85f, k = 0.3f, alpha = 0.02f;
    };

    /** The model's parameters for a tape type and a bias (0..1, 0.5 is the
        calibrated bias). */
    static Params paramsFor (TapeType type, float bias01) noexcept
    {
        const float b = bias01 < 0.0f ? 0.0f : (bias01 > 1.0f ? 1.0f : bias01);
        float k0 = 0.3f, alpha = 0.02f;
        switch (type)
        {
            case TapeType::modern:     k0 = 0.30f; alpha = 0.02f; break;
            case TapeType::vintage:    k0 = 0.45f; alpha = 0.05f; break;
            case TapeType::highOutput: k0 = 0.25f; alpha = 0.02f; break;
        }
        Params p;
        p.c = b < 0.5f ? 0.4f + 0.9f * b : 0.85f + 0.24f * (b - 0.5f);   // 0.4, 0.85, 0.97
        p.k = k0 * (1.5f - b);                                              // wider under-biased
        p.alpha = alpha;
        return p;
    }

    /** How many dB above the modern type's level a type saturates. */
    static float headroomDb (TapeType type) noexcept
    {
        switch (type)
        {
            case TapeType::modern:     return 0.0f;
            case TapeType::vintage:    return -4.0f;
            case TapeType::highOutput: return 4.0f;
        }
        return 0.0f;
    }

    /** The type's hiss, in dB against the modern type's. */
    static float hissOffsetDb (TapeType type) noexcept
    {
        switch (type)
        {
            case TapeType::modern:     return 0.0f;
            case TapeType::vintage:    return 6.0f;
            case TapeType::highOutput: return -3.0f;
        }
        return 0.0f;
    }

    /** The gain near the reference level (H amplitude 0.1), for normalising
        the output: c / (1 - alpha c), plus the irreversible part's share. */
    static float approxGain (const Params& p) noexcept
    {
        return p.c / (1.0f - p.alpha * p.c) + (1.0f - p.c) * 0.03f / (p.k > 0.01f ? p.k : 0.01f);
    }

    //==========================================================================
    void prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 96000.0;
        T = 1.0 / sampleRate;
        reset();
    }

    void reset() noexcept
    {
        M = 0.0;
        hPrev = 0.0;
        hdPrev = 0.0;
    }

    void setParams (const Params& p) noexcept
    {
        c = p.c < 0.0f ? 0.0 : (p.c > 0.999f ? 0.999 : (double) p.c);
        k = p.k > 1.0e-3f ? (double) p.k : 1.0e-3;
        alpha = p.alpha > 0.0f ? (double) p.alpha : 0.0;
    }

    /** One field sample H in, the magnetisation M (about -1..1) out. */
    float process (float hIn) noexcept
    {
        const double H = (double) sanitiseSample (hIn, 100.0f);
        const double Hd = (1.0 + derivAlpha) * sampleRate * (H - hPrev) - derivAlpha * hdPrev;
        const double Hm = 0.5 * (H + hPrev), Hdm = 0.5 * (Hd + hdPrev);

        const double k1 = T * dMdt (M, hPrev, hdPrev);
        const double k2 = T * dMdt (M + 0.5 * k1, Hm, Hdm);
        const double k3 = T * dMdt (M + 0.5 * k2, Hm, Hdm);
        const double k4 = T * dMdt (M + k3, H, Hd);
        M += (k1 + 2.0 * k2 + 2.0 * k3 + k4) / 6.0;

        if (! (std::abs (M) <= 2.0))   // NaN too
        {
            reset();
            return 0.0f;
        }

        hPrev = H;
        hdPrev = std::abs (Hd) < 1.0e30 ? Hd : 0.0;
        return (float) M;
    }

    float getM() const noexcept { return (float) M; }
    float getH() const noexcept { return (float) hPrev; }

private:
    static constexpr double Ms = 1.0, a = 1.0 / 3.0;
    static constexpr double derivAlpha = 0.9;

    double dMdt (double m, double H, double Hd) const noexcept
    {
        const double Q = (H + alpha * m) / a;
        double L, Lp;
        if (std::abs (Q) < 1.0e-3)
        {
            L = Q / 3.0;
            Lp = 1.0 / 3.0;
        }
        else
        {
            const double coth = 1.0 / std::tanh (Q);
            L = coth - 1.0 / Q;
            Lp = 1.0 / (Q * Q) - coth * coth + 1.0;
        }

        const double mDiff = Ms * L - m;
        const double d = Hd >= 0.0 ? 1.0 : -1.0;
        const double dm = (d > 0.0) == (mDiff > 0.0) ? 1.0 : 0.0;

        double den = (1.0 - c) * d * k - alpha * mDiff;
        if (std::abs (den) < 1.0e-9)
            den = den < 0.0 ? -1.0e-9 : 1.0e-9;

        const double f1 = dm * (1.0 - c) * mDiff / den;
        const double f2 = c * Ms / a * Lp;
        const double f3 = 1.0 - alpha * c * Ms / a * Lp;
        return Hd * (f1 + f2) / f3;
    }

    double sampleRate = 96000.0, T = 1.0 / 96000.0;
    double c = 0.85, k = 0.3, alpha = 0.02;
    double M = 0.0, hPrev = 0.0, hdPrev = 0.0;
};

} // namespace fxme
