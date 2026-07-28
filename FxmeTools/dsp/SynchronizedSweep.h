/*
  ------------------------------------------------------------------------------
    SynchronizedSweep.h

    Synchronized swept-sine system identification, after:
      A. Novak, P. Lotton, L. Simon, "Synchronized Swept-Sine: Theory,
      Application, and Implementation", J. Audio Eng. Soc. 63(10), 2015.

    The stimulus is the exponential (log) sweep
        x(t) = sin( 2*pi*f1*L * (exp(t/L) - 1) ),
    with the sweep rate L QUANTIZED so that f1*L is an integer:
        L = round(f1*T / ln(f2/f1)) / f1        (T = desired duration).
    With that choice the sweep's phase at every harmonic arrival time
    dt_m = L*ln(m) is an exact multiple of 2*pi, so when the recorded
    response is deconvolved by the sweep's analytic spectral inverse, the
    impulse responses of the harmonic orders separate cleanly in time AND
    keep their true phase:

        full IR = deconvolve(recorded):
          - the LINEAR impulse response sits at the system's propagation
            delay (near index 0 for a small delay);
          - the order-m harmonic IR sits L*ln(m)*fs samples BEFORE it,
            i.e. circularly wrapped towards the end of the buffer.

    The actual sweep duration L*ln(f2/f1) differs slightly from the
    requested one because of the quantization; getDurationS() returns it.
    The deconvolution also works for a non-quantized L (prepareExact with a
    legacy recording's L): the harmonic separation in time still holds, only
    the harmonic PHASE alignment is lost.

    Everything is message-thread, allocation-friendly code (file-based
    analysis, not realtime).

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <complex>
#include <vector>

namespace fxme
{

class SynchronizedSweep
{
public:
    SynchronizedSweep() = default;

    //==========================================================================
    /** The synchronized sweep rate (seconds): L = k/f1 with
        k = round(f1*targetDurationS / ln(f2/f1)), k >= 1. */
    static double synchronizedL (double f1, double f2, double targetDurationS)
    {
        const double k = std::max (1.0, std::round (f1 * targetDurationS
                                                    / std::log (f2 / f1)));
        return k / f1;
    }

    /** Configures a synchronized sweep: L is quantized from the target
        duration (the actual duration becomes L*ln(f2/f1)). */
    void prepare (double newF1, double newF2, double newSampleRate,
                  double targetDurationS)
    {
        prepareExact (newF1, newF2, newSampleRate,
                      synchronizedL (newF1, newF2, targetDurationS));
    }

    /** Configures with an explicit sweep rate L — e.g. read back from a
        measurement manifest. No quantization is applied. */
    void prepareExact (double newF1, double newF2, double newSampleRate,
                       double newL)
    {
        f1 = newF1;
        f2 = newF2;
        fs = newSampleRate;
        L  = newL;
    }

    double getF1() const noexcept           { return f1; }
    double getF2() const noexcept           { return f2; }
    double getSampleRate() const noexcept   { return fs; }
    double getL() const noexcept            { return L; }

    /** Exact sweep duration, L*ln(f2/f1), in seconds. */
    double getDurationS() const noexcept    { return L * std::log (f2 / f1); }

    /** Sweep length in samples (duration rounded to the nearest sample). */
    int getNumSamples() const noexcept
    {
        return (int) std::llround (getDurationS() * fs);
    }

    //==========================================================================
    /** Sample n of the sweep (no fade — apply your own envelope if needed;
        a short fade-out only blurs the extreme top of the band). */
    float getSample (int n) const noexcept
    {
        const double t = (double) n / fs;
        return (float) std::sin (juce::MathConstants<double>::twoPi * f1 * L
                                 * (std::exp (t / L) - 1.0));
    }

    /** The whole sweep, getNumSamples() long. */
    std::vector<float> render() const
    {
        std::vector<float> out ((size_t) std::max (0, getNumSamples()));
        for (size_t n = 0; n < out.size(); ++n)
            out[n] = getSample ((int) n);
        return out;
    }

    //==========================================================================
    /** Deconvolves a recorded response by the sweep's ANALYTIC spectral
        inverse (Novak 2015, the stationary-phase approximation of the sweep
        spectrum):

            Xinv(f) = 2*sqrt(f/L) * exp(-j*2*pi*f*L*(1 - ln(f/f1)) + j*pi/4)

        The recording must be time-aligned with the sweep's start (sample 0 of
        `recorded` is where the sweep's sample 0 was emitted) and should
        include the decay tail. Returns the full higher-order impulse response
        of length getFftSizeFor(numRecorded):

          - linear IR at the system's propagation delay,
          - order-m harmonic IR at harmonicOffsetSamples(m) before it
            (circularly wrapped),

        normalized so that an identity system yields a (band-limited) unit
        pulse. */
    std::vector<float> deconvolve (const float* recorded, int numRecorded) const
    {
        const int N = getFftSizeFor (numRecorded);
        if (N == 0 || recorded == nullptr)
            return {};

        int order = 0;
        while ((1 << order) < N)
            ++order;

        juce::dsp::FFT fft (order);
        std::vector<std::complex<float>> spec ((size_t) N), work ((size_t) N);
        for (int i = 0; i < numRecorded; ++i)
            work[(size_t) i] = { recorded[i], 0.0f };
        fft.perform (work.data(), spec.data(), false);

        // Multiply by the analytic inverse (Hermitian-symmetric so the
        // result stays real). The 1/fs converts the continuous-time inverse
        // to the DFT's amplitude convention.
        const double df = fs / (double) N;
        spec[0] = { 0.0f, 0.0f };
        for (int k = 1; k <= N / 2; ++k)
        {
            const double f = k * df;
            const double mag = 2.0 * std::sqrt (f / L) / fs;
            const double ph  = -juce::MathConstants<double>::twoPi * f * L
                                   * (1.0 - std::log (f / f1))
                               + juce::MathConstants<double>::pi / 4.0;
            const std::complex<float> c ((float) (mag * std::cos (ph)),
                                         (float) (mag * std::sin (ph)));
            spec[(size_t) k] *= c;
            if (k < N / 2)
                spec[(size_t) (N - k)] *= std::conj (c);
        }

        fft.perform (spec.data(), work.data(), true);

        std::vector<float> out ((size_t) N);
        for (int i = 0; i < N; ++i)
            out[(size_t) i] = work[(size_t) i].real();
        return out;
    }

    /** FFT size deconvolve() will use for a recording of that length: the
        next power of two of max(recording, sweep) — comfortably larger than
        the harmonic offsets L*ln(m)*fs, so the wrapped harmonic IRs land far
        from the linear one. */
    int getFftSizeFor (int numRecorded) const noexcept
    {
        const int need = std::max (numRecorded, getNumSamples());
        if (need <= 0)
            return 0;
        int n = 1;
        while (n < need)
            n <<= 1;
        return n;
    }

    /** How many samples BEFORE the linear IR the order-m harmonic IR sits:
        dt_m = L*ln(m)*fs. Order 1 gives 0. */
    double harmonicOffsetSamples (int order) const noexcept
    {
        return L * std::log ((double) order) * fs;
    }

    /** Circularly extracts `length` samples of `fullIr` centred on
        `centreIndex` (any real index; wraps around). Typical use: the
        order-m harmonic IR is
            extractCircular (full, linearPeakIndex - harmonicOffsetSamples (m),
                             length);
        with `length` small enough not to overlap the neighbouring orders
        (their spacing shrinks as L*ln(m/(m-1))*fs). */
    static std::vector<float> extractCircular (const std::vector<float>& fullIr,
                                               double centreIndex, int length)
    {
        std::vector<float> out ((size_t) std::max (0, length), 0.0f);
        const int N = (int) fullIr.size();
        if (N == 0)
            return out;

        const int c = (int) std::llround (centreIndex);
        for (int i = 0; i < length; ++i)
        {
            int idx = (c - length / 2 + i) % N;
            if (idx < 0)
                idx += N;
            out[(size_t) i] = fullIr[(size_t) idx];
        }
        return out;
    }

private:
    double f1 = 10.0, f2 = 20000.0;
    double fs = 48000.0;
    double L  = 1.0;

    JUCE_LEAK_DETECTOR (SynchronizedSweep)
};

} // namespace fxme
