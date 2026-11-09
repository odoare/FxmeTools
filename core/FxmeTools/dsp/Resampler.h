/*
  ------------------------------------------------------------------------------
    Resampler.h

    Sample-rate conversion, JUCE-free.

      Resampler            one direction, streaming, any ratio: push input
                           samples at one rate, get output samples at the
                           other. Each output sample is the input signal
                           band-limited and read exactly at its own time
                           (output j at input time j * inRate / outRate).
      ResamplingContainer  the round trip around a process that must run at
                           a fixed rate (a neural amp model trained at 48 kHz
                           in a 44.1 or 96 kHz host): host block in, the
                           process called on the same audio at its rate, host
                           block out, the same number of samples, with a fixed
                           and exactly integer latency. At equal rates it is
                           a pass-through with no latency.

    The filter: a windowed sinc (Kaiser window, beta 9), its cutoff at 0.952
    of the lower of the two Nyquist frequencies, 64 zero crossings each side
    (all adjustable in prepare()). Polyphase: at prepare() the kernel is
    tabulated for 256 fractional positions over the taps it needs, and each
    output is two dot products over contiguous memory (the history is kept
    twice over, so any window is contiguous), linearly interpolated between
    the two neighbouring phases: every ratio, rational or not, goes through
    the same loop, which the compiler vectorises. Measured (see
    core/tests/CoreResamplerTests.cpp): flat to 20 kHz between 44.1 and
    48 kHz, aliasing under -100 dB, the round trip's error -100 dB; the
    round trip around 48 kHz costs 0.3 to 0.55 % of one core at 44.1 kHz,
    about 1 % at 96 kHz, 1 to 2 % at 192 kHz (i7-10810U, -O2 to -O3
    -march=native).

    Latency of the round trip around 48 kHz: 139 samples at 44.1 kHz,
    275 at 96 kHz, 547 at 192 kHz (about 3 ms; the price of a flat passband
    to 20 kHz with nothing folding back).

    Why integer latency: a dry / wet mix of the converted path with the dry
    signal (delayed by the reported latency) must line up to the sample, or
    the mix comb-filters the top octave. Each output sample is computed at
    exactly its own host time, then delayed by a whole number of samples.

    Real time: prepare() allocates; process() never does, nor locks. Mono:
    one instance per channel.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace fxme
{

class Resampler
{
public:
    static constexpr int numPhases = 256;
    static constexpr int defaultZeros = 64;
    static constexpr double defaultCutoff = 0.952, defaultBeta = 9.0;

    /** `maxInputBlock`: the most samples one process() call is given.
        `cutoff`: of the lower Nyquist. `zeros`: zero crossings each side.
        `beta`: the Kaiser window's shape. Allocates. */
    void prepare (double inRateIn, double outRateIn, int maxInputBlock, int zeros = defaultZeros,
                  double cutoff = defaultCutoff, double beta = defaultBeta)
    {
        inRate = inRateIn;
        outRate = outRateIn;
        step = inRate / outRate;
        // The cutoff in cycles per input sample (2 fc), under the lower Nyquist.
        twoFc = cutoff * std::min (1.0, outRate / inRate);
        halfWidth = (double) std::max (4, zeros) / twoFc;          // input samples
        halfTaps = (int) std::ceil (halfWidth);
        // The window spans 2 halfTaps inputs; rows are padded with zeros to a
        // multiple of `lanes` (the dot products' independent partial sums).
        taps = ((2 * halfTaps + lanes - 1) / lanes) * lanes;

        // Phase row p is the kernel for an output p / numPhases of an input
        // sample after `base`, over the inputs base - halfTaps + 1 .. base + halfTaps.
        rows.assign ((size_t) ((numPhases + 1) * taps), 0.0f);
        const double i0Beta = besselI0 (beta);
        for (int p = 0; p <= numPhases; ++p)
            for (int j = 0; j < taps; ++j)
            {
                const double u = (double) p / numPhases + halfTaps - 1 - j;   // output time - input time
                if (std::abs (u) >= halfWidth)
                    continue;
                const double r = u / halfWidth;
                const double x = twoFc * u;
                const double sinc = x == 0.0 ? 1.0 : std::sin (pi * x) / (pi * x);
                rows[(size_t) (p * taps + j)] = (float) (twoFc * sinc * besselI0 (beta * std::sqrt (1.0 - r * r)) / i0Beta);
            }

        // The history, mirrored (each sample written twice, `size` apart) so
        // that any window of `taps` inputs is contiguous.
        const int needed = taps + maxInputBlock + 8;
        size = 64;
        while (size < needed)
            size <<= 1;
        history.assign ((size_t) (2 * size), 0.0f);
        mask = size - 1;
        reset();
    }

    void reset()
    {
        std::fill (history.begin(), history.end(), 0.0f);
        inCount = 0;
        outCount = 0;
    }

    /** Pushes `numIn` input samples (at most the prepared maximum), writes
        the outputs they complete to `out` (room for maxOutputFor (numIn))
        and returns how many. */
    int process (const float* in, int numIn, float* out) noexcept
    {
        for (int i = 0; i < numIn; ++i)
        {
            const auto w = (size_t) ((inCount + i) & mask);
            history[w] = in[i];
            history[w + (size_t) size] = in[i];
        }
        inCount += numIn;

        int produced = 0;
        for (;;)
        {
            const double t = (double) outCount * step;   // the output's time, in input samples
            const auto base = (std::int64_t) std::floor (t);
            if (base + halfTaps >= inCount)
                break;

            const double phase = (t - (double) base) * numPhases;
            const int p = std::min ((int) phase, numPhases - 1);
            const float a = (float) (phase - p);
            const float* x = history.data() + ((base - halfTaps + 1) & mask);
            const float* r0 = rows.data() + (size_t) (p * taps);
            const float* r1 = r0 + taps;
            // One coefficient per tap, interpolated between the two phases,
            // into `lanes` independent partial sums: the compiler vectorises
            // them without being allowed to reorder a single sum (no
            // fast-math). Of the forms measured, the fastest at -O2, -O3 and
            // -march=native alike.
            float acc[lanes] = {};
            for (int j = 0; j < taps; j += lanes)
                for (int l = 0; l < lanes; ++l)
                    acc[l] += x[j + l] * (r0[j + l] + a * (r1[j + l] - r0[j + l]));
            float sum = 0.0f;
            for (int l = 0; l < lanes; ++l)
                sum += acc[l];
            out[produced++] = sum;
            ++outCount;
        }
        return produced;
    }

    /** The most outputs one call with `numIn` inputs can produce. */
    int maxOutputFor (int numIn) const noexcept { return (int) std::ceil ((double) numIn / step) + 2; }

    /** How far behind its input an output is produced, in input samples. */
    double getLatencyInInputSamples() const noexcept  { return (double) halfTaps; }
    double getInputRate() const noexcept  { return inRate; }
    double getOutputRate() const noexcept { return outRate; }

private:
    static constexpr double pi = 3.14159265358979323846;
    static constexpr int lanes = 8;

    static double besselI0 (double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 60; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
            if (term < 1.0e-14 * sum)
                break;
        }
        return sum;
    }

    double inRate = 48000.0, outRate = 48000.0, step = 1.0, twoFc = 0.95, halfWidth = 64.0;
    int halfTaps = 64, taps = 128, size = 64;
    std::vector<float> rows, history;
    std::int64_t mask = 0, inCount = 0, outCount = 0;
};

//==============================================================================
/** Runs a fixed-rate process inside a host at another rate. */
class ResamplingContainer
{
public:
    /** `maxHostBlock`: the most samples a process() call is given. */
    void prepare (double hostRateIn, double innerRateIn, int maxHostBlock, int zeros = Resampler::defaultZeros,
                  double cutoff = Resampler::defaultCutoff)
    {
        hostRate = hostRateIn;
        innerRate = innerRateIn;
        maxHost = maxHostBlock;
        bypass = std::abs (hostRate - innerRate) < 1.0e-6;

        if (bypass)
        {
            maxInner = maxHostBlock;
            latency = 0;
            innerIn.assign ((size_t) maxHostBlock, 0.0f);
            innerOut.assign ((size_t) maxHostBlock, 0.0f);
            return;
        }

        down.prepare (hostRate, innerRate, maxHostBlock, zeros, cutoff);
        maxInner = down.maxOutputFor (maxHostBlock);
        up.prepare (innerRate, hostRate, maxInner, zeros, cutoff);

        // Host sample k is computed at exactly host time k; it is ready once
        // the host input reaches k + both filters' half widths (the
        // up-converter's in host samples) + one inner sample (in host
        // samples) + the rounding of each stage.
        const double hostPerInner = hostRate / innerRate;
        latency = (int) std::ceil (down.getLatencyInInputSamples()
                                   + up.getLatencyInInputSamples() * hostPerInner + hostPerInner) + 2;

        innerIn.assign ((size_t) maxInner, 0.0f);
        innerOut.assign ((size_t) maxInner, 0.0f);
        upOut.assign ((size_t) up.maxOutputFor (maxInner), 0.0f);

        int size = 64;
        while (size < latency + 4 * maxHostBlock + 16)
            size <<= 1;
        fifo.assign ((size_t) size, 0.0f);
        fifoMask = size - 1;
        reset();
    }

    void reset()
    {
        if (bypass)
            return;
        down.reset();
        up.reset();
        std::fill (fifo.begin(), fifo.end(), 0.0f);
        fifoRead = 0;
        fifoWrite = latency;   // the first `latency` outputs are silence
        underruns = 0;
    }

    /** Runs `inner (const float* in, float* out, int n)` at the inner rate on
        the host block `io` (in place, `numSamples` at most the prepared
        maximum); `n` is at most getMaxInnerBlock() and may be 0. */
    template <typename Inner>
    void process (float* io, int numSamples, Inner&& inner) noexcept
    {
        if (bypass)
        {
            std::copy (io, io + numSamples, innerIn.data());
            inner (innerIn.data(), io, numSamples);
            return;
        }

        const int m = down.process (io, numSamples, innerIn.data());
        if (m > 0)
            inner (innerIn.data(), innerOut.data(), m);
        const int p = up.process (innerOut.data(), m, upOut.data());
        for (int i = 0; i < p; ++i)
            fifo[(size_t) ((fifoWrite + i) & fifoMask)] = upOut[(size_t) i];
        fifoWrite += p;

        for (int i = 0; i < numSamples; ++i)
        {
            if (fifoRead < fifoWrite)
                io[i] = fifo[(size_t) (fifoRead++ & fifoMask)];
            else
            {
                io[i] = 0.0f;   // never with the computed latency (the tests check it)
                ++underruns;
            }
        }
    }

    /** The delay the round trip adds, in host samples: exact, an integer. */
    int getLatencySamples() const noexcept     { return latency; }
    int getMaxInnerBlock() const noexcept      { return maxInner; }
    bool isBypassed() const noexcept           { return bypass; }
    std::int64_t getUnderruns() const noexcept { return underruns; }

private:
    double hostRate = 48000.0, innerRate = 48000.0;
    int maxHost = 0, maxInner = 0, latency = 0;
    bool bypass = true;
    Resampler down, up;
    std::vector<float> innerIn, innerOut, upOut, fifo;
    std::int64_t fifoMask = 0, fifoRead = 0, fifoWrite = 0, underruns = 0;
};

} // namespace fxme
