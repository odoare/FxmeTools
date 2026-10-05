/*
  ------------------------------------------------------------------------------
    TptSvf.h

    The trapezoidal (topology-preserving transform) state-variable filter, in
    Andrew Simper's (Cytomic) form, plus a cascade of them for the steep
    Butterworth high- and low-passes of an EQ.

    Why this rather than fxme::Biquad: the SVF's state is the voltage on two
    integrators, not a delayed copy of past outputs, so its cutoff and damping
    can change on every sample without the bursts a direct-form biquad emits
    when its coefficients are swept. It is the filter to use when a cutoff is
    modulated or smoothed per sample (a dynamic EQ, a filter envelope), and
    for steady filters it costs about the same.

    One TptSvf is one channel. Coefficients come from setup (g, k):
        g = tan (pi fc / fs)    (prewarped cutoff, gFor() clamps fc)
        k = 1 / Q               (damping)
    and every output (low, band, high, the normalised band-pass k * band) is
    available from one tick.

    The response helpers evaluate the analog prototype on the prewarped axis,
    s = j tan (pi f / fs) / g, which is exactly the digital filter's response
    (the bilinear transform maps one onto the other). A GUI drawing a curve
    uses them; no transfer-function algebra in z is needed.

    Header-only, no allocation, realtime safe.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/util/Math.h>
#include <array>
#include <cmath>
#include <complex>

namespace fxme
{

class TptSvf
{
public:
    struct Outputs
    {
        float low, band, high;
    };

    /** g for a cutoff `hz` at `sampleRate`, the cutoff clamped to
        [1 Hz, 0.49 fs] so the tangent stays finite. */
    static double gFor (double hz, double sampleRate) noexcept
    {
        const double f = fxme::jlimit (1.0, 0.49 * sampleRate, hz);
        return std::tan (MathConstants<double>::pi * f / sampleRate);
    }

    void setup (double g, double k) noexcept
    {
        gCoef = g;
        kCoef = k;
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    void reset() noexcept { ic1 = ic2 = 0.0; }

    Outputs tick (float x) noexcept
    {
        const double v0 = x;
        const double v3 = v0 - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        return { (float) v2, (float) v1, (float) (v0 - kCoef * v1 - v2) };
    }

    float lowPass (float x) noexcept   { return tick (x).low; }
    float highPass (float x) noexcept  { return tick (x).high; }

    /** Band-pass with 0 dB at its centre (k times the raw band output). */
    float bandPass (float x) noexcept  { return (float) kCoef * tick (x).band; }

    double getG() const noexcept { return gCoef; }
    double getK() const noexcept { return kCoef; }

    //==========================================================================
    // Responses of the analog prototype at the normalised, prewarped frequency
    // w = tan (pi f / fs) / g (see the file comment).

    static std::complex<double> lowPassResponse (double w, double k) noexcept
    {
        const std::complex<double> s (0.0, w);
        return 1.0 / (s * s + k * s + 1.0);
    }

    static std::complex<double> highPassResponse (double w, double k) noexcept
    {
        const std::complex<double> s (0.0, w);
        return (s * s) / (s * s + k * s + 1.0);
    }

    static std::complex<double> bandPassResponse (double w, double k) noexcept
    {
        const std::complex<double> s (0.0, w);
        return (k * s) / (s * s + k * s + 1.0);
    }

    /** w for a frequency `hz` against a filter of coefficient `g`. */
    static double normalisedFrequency (double hz, double sampleRate, double g) noexcept
    {
        const double f = fxme::jlimit (0.0, 0.4999 * sampleRate, hz);
        return std::tan (MathConstants<double>::pi * f / sampleRate) / fxme::jmax (1.0e-12, g);
    }

private:
    double gCoef = 0.0, kCoef = 1.4142135623730951;
    double a1 = 1.0, a2 = 0.0, a3 = 0.0;
    double ic1 = 0.0, ic2 = 0.0;
};

//==============================================================================
/** A high- or low-pass of 12, 24 or 48 dB per octave: one, two or four SVF
    sections with Butterworth dampings. `resonance` scales the Q of the last
    (sharpest) section, so 1 is a plain Butterworth and the 12 dB slope with
    resonance r has Q = 0.7071 r.

    The cutoff is smoothed (a linear ramp of its logarithm over
    `smoothingSeconds`, 20 ms by default) so automation does not zipper.
    Channels share the coefficients: call advance() once per sample, then
    process() for each channel. */
class SvfPassCascade
{
public:
    static constexpr int maxSections = 4;
    static constexpr int maxChannels = 2;

    enum class Type { highPass, lowPass };

    /** Number of sections for a slope index: 0 = 12 dB/oct, 1 = 24, 2 = 48. */
    static int sectionsForSlope (int slopeIndex) noexcept
    {
        return slopeIndex <= 0 ? 1 : (slopeIndex == 1 ? 2 : 4);
    }

    /** The damping (1 / Q) of section `i` of an `n`-section Butterworth
        cascade (order 2n): 2 sin ((2i + 1) pi / (4n)). */
    static double butterworthK (int i, int n) noexcept
    {
        return 2.0 * std::sin ((2.0 * i + 1.0) * MathConstants<double>::pi / (4.0 * n));
    }

    void prepare (double newSampleRate, float smoothingSeconds = 0.02f) noexcept
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
        smoothSteps = fxme::jmax (1, (int) std::floor (smoothingSeconds * sampleRate));
        logFreq = logTarget;
        stepsLeft = 0;
        updateCoefficients();
        reset();
    }

    void reset() noexcept
    {
        for (auto& ch : sections)
            for (auto& s : ch)
                s.reset();
    }

    void setType (Type t) noexcept
    {
        if (t != type) { type = t; reset(); }
    }

    /** 1, 2 or 4 sections. Resets the filter when it changes. */
    void setNumSections (int n) noexcept
    {
        n = n <= 1 ? 1 : (n == 2 ? 2 : 4);
        if (n != numSections) { numSections = n; reset(); updateCoefficients(); }
    }

    void setResonance (float r) noexcept
    {
        r = fxme::jlimit (0.1f, 30.0f, r);
        if (r != resonance) { resonance = r; updateCoefficients(); }
    }

    /** The cutoff to glide to (snaps when `snap`). */
    void setFrequency (float hz, bool snap = false) noexcept
    {
        const double target = std::log (fxme::jlimit (1.0, 0.49 * sampleRate, (double) hz));
        if (snap)
        {
            logTarget = logFreq = target;
            stepsLeft = 0;
            updateCoefficients();
            return;
        }
        if (target == logTarget)
            return;
        logTarget = target;
        stepsLeft = smoothSteps;
        logStep = (logTarget - logFreq) / stepsLeft;
    }

    /** Moves the cutoff one sample along its glide. */
    void advance() noexcept
    {
        if (stepsLeft <= 0)
            return;
        if (--stepsLeft == 0)
            logFreq = logTarget;
        else
            logFreq += logStep;
        updateCoefficients();
    }

    float process (int channel, float x) noexcept
    {
        auto& ch = sections[(size_t) channel];
        for (int i = 0; i < numSections; ++i)
            x = type == Type::lowPass ? ch[(size_t) i].lowPass (x) : ch[(size_t) i].highPass (x);
        return x;
    }

    /** Magnitude (linear) of such a cascade at `hz`. For drawing. */
    static double magnitude (Type type, int numSections, double fcHz, double resonance,
                             double hz, double sampleRate) noexcept
    {
        const double g = TptSvf::gFor (fcHz, sampleRate);
        const double w = TptSvf::normalisedFrequency (hz, sampleRate, g);
        double m = 1.0;
        for (int i = 0; i < numSections; ++i)
        {
            const double k = sectionK (i, numSections, resonance);
            m *= std::abs (type == Type::lowPass ? TptSvf::lowPassResponse (w, k)
                                                 : TptSvf::highPassResponse (w, k));
        }
        return m;
    }

private:
    static double sectionK (int i, int n, double resonance) noexcept
    {
        const double k = butterworthK (i, n);
        // Sections are ordered from the most damped to the sharpest; only the
        // sharpest carries the resonance.
        return i == n - 1 ? k / fxme::jmax (0.1, resonance) : k;
    }

    void updateCoefficients() noexcept
    {
        const double g = TptSvf::gFor (std::exp (logFreq), sampleRate);
        for (auto& ch : sections)
            for (int i = 0; i < numSections; ++i)
                ch[(size_t) i].setup (g, sectionK (i, numSections, resonance));
    }

    double sampleRate = 48000.0;
    Type type = Type::highPass;
    int numSections = 1;
    float resonance = 1.0f;

    double logFreq = std::log (1000.0), logTarget = std::log (1000.0), logStep = 0.0;
    int stepsLeft = 0, smoothSteps = 960;

    std::array<std::array<TptSvf, maxSections>, maxChannels> sections {};
};

} // namespace fxme
