/*
  ------------------------------------------------------------------------------
    util/Fft.h

    Framework-free radix-2 FFT replacing JUCE's dsp::FFT in the core DSP.

    Semantics are deliberately identical to JUCE's, so migrating a call site is
    a change of type name and nothing else:

      - bins come out in natural order (bin k is frequency k * fs / N),
      - the forward transform is unscaled,
      - the inverse transform is scaled by 1/N,
      - the real-only transforms use the same 2N-float buffer layout, and the
        inverse reconstructs the negative frequencies from the positive ones
        exactly as JUCE's fallback engine does.

    Why not WDL's FFT, which this repository already vendors: it stops at
    N = 32768, and the swept-sine deconvolution in dsp/SynchronizedSweep.h sizes
    its transform from the recording length — a 10 s sweep at 48 kHz already
    needs 2^19, and 30 s at 96 kHz needs 2^22. WDL is still the right tool for
    the convolution engine, which never exceeds its limit.

    This is a plain scalar implementation chosen for clarity and for having no
    dependencies. It is in the same performance class as JUCE's own fallback
    engine (what Linux and Windows builds already use), and slower than the
    vDSP path JUCE takes on macOS. If that ever matters, a SIMD backend can be
    dropped in behind this interface without touching a single call site —
    which is the point of matching JUCE's semantics so exactly.

    Allocation happens in the constructor only; the transforms allocate
    nothing, take no locks and throw nothing, so they are safe on the audio
    thread. Not thread-safe: give each thread its own instance.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace fxme
{

//==============================================================================
/** Complex-to-complex radix-2 FFT. Mirrors JUCE's dsp::FFT::perform. */
class Fft
{
public:
    /** Builds the twiddle table for a transform of 2^order points. */
    explicit Fft (int fftOrder)
        : order (fftOrder < 0 ? 0 : fftOrder),
          size (1 << (fftOrder < 0 ? 0 : fftOrder))
    {
        // Generated in double and stored as float: at the sizes the sweep
        // deconvolution reaches (2^22), accumulating the angle in float would
        // visibly degrade the upper bins.
        twiddles.resize (static_cast<std::size_t> (size / 2));

        for (int i = 0; i < size / 2; ++i)
        {
            const double angle = -2.0 * pi * static_cast<double> (i) / static_cast<double> (size);
            twiddles[static_cast<std::size_t> (i)] = { static_cast<float> (std::cos (angle)),
                                                       static_cast<float> (std::sin (angle)) };
        }
    }

    int getSize()  const noexcept { return size; }
    int getOrder() const noexcept { return order; }

    /** Out-of-place transform. `input` and `output` hold getSize() values and
        must not overlap. Forward is unscaled; inverse is scaled by 1/N. */
    void perform (const std::complex<float>* input,
                  std::complex<float>* output,
                  bool inverse) const noexcept
    {
        if (size == 1)
        {
            output[0] = input[0];
            return;
        }

        for (int i = 0; i < size; ++i)
            output[i] = input[i];

        bitReverseInPlace (output);

        for (int len = 2; len <= size; len <<= 1)
        {
            const int half = len >> 1;
            const int step = size / len;

            for (int base = 0; base < size; base += len)
            {
                for (int j = 0; j < half; ++j)
                {
                    const auto w0 = twiddles[static_cast<std::size_t> (j * step)];
                    const std::complex<float> w = inverse ? std::conj (w0) : w0;

                    const auto u = output[base + j];
                    const auto v = output[base + j + half] * w;

                    output[base + j]        = u + v;
                    output[base + j + half] = u - v;
                }
            }
        }

        if (inverse)
        {
            const float scale = 1.0f / static_cast<float> (size);
            for (int i = 0; i < size; ++i)
                output[i] *= scale;
        }
    }

private:
    static constexpr double pi = 3.141592653589793238;

    /** Decimation-in-time needs the input in bit-reversed order. Computed
        incrementally rather than from a table: at 2^22 the table would be
        16 MB of cache-hostile indirection to save a handful of operations. */
    void bitReverseInPlace (std::complex<float>* data) const noexcept
    {
        for (int i = 1, j = 0; i < size; ++i)
        {
            int bit = size >> 1;

            for (; (j & bit) != 0; bit >>= 1)
                j ^= bit;

            j ^= bit;

            if (i < j)
            {
                const auto tmp = data[i];
                data[i] = data[j];
                data[j] = tmp;
            }
        }
    }

    int order;
    int size;
    std::vector<std::complex<float>> twiddles;
};

//==============================================================================
/** Real-input transforms over the same 2N-float buffer layout JUCE uses.

    Every entry point takes a buffer of 2 * getSize() floats. For a forward
    transform the first getSize() floats are the input samples, and on return
    the buffer holds getSize() interleaved complex bins. */
class RealFft
{
public:
    explicit RealFft (int fftOrder)
        : fft (fftOrder),
          scratch (static_cast<std::size_t> (fft.getSize()))
    {
    }

    int getSize()  const noexcept { return fft.getSize(); }
    int getOrder() const noexcept { return fft.getOrder(); }

    /** Real forward transform, in place over 2N floats.

        `onlyCalculateNonNegativeFrequencies` is a hint and is ignored, exactly
        as JUCE's fallback engine ignores it: all N bins are always written, so
        the result satisfies callers passing either value. */
    void performRealOnlyForwardTransform (float* inputOutputData,
                                          bool onlyCalculateNonNegativeFrequencies = false) const noexcept
    {
        (void) onlyCalculateNonNegativeFrequencies;

        const int size = getSize();

        if (size == 1)
            return;

        for (int i = 0; i < size; ++i)
            scratch[static_cast<std::size_t> (i)] = { inputOutputData[i], 0.0f };

        fft.perform (scratch.data(),
                     reinterpret_cast<std::complex<float>*> (inputOutputData),
                     false);
    }

    /** Inverse of the above. Only the first (N/2 + 1) bins are read — the rest
        are rebuilt by conjugate symmetry — but the buffer must still be 2N
        floats. On return the first N floats are the reconstituted samples. */
    void performRealOnlyInverseTransform (float* inputOutputData) const noexcept
    {
        const int size = getSize();

        if (size == 1)
            return;

        auto* spectrum = reinterpret_cast<std::complex<float>*> (inputOutputData);

        // Mirrors the positive half onto the negative one. At i == size/2 this
        // conjugates the Nyquist bin with itself, forcing it real — which is
        // what makes the reconstruction come out real.
        for (int i = size >> 1; i < size; ++i)
            spectrum[i] = std::conj (spectrum[size - i]);

        fft.perform (spectrum, scratch.data(), true);

        for (int i = 0; i < size; ++i)
        {
            inputOutputData[i]        = scratch[static_cast<std::size_t> (i)].real();
            inputOutputData[i + size] = scratch[static_cast<std::size_t> (i)].imag();
        }
    }

    /** Forward transform reduced to a magnitude spectrum, for displays and
        analysis. Writes `limit` magnitudes to the start of the buffer and
        zeroes the remainder, where limit is N/2 + 1 when only the non-negative
        frequencies were asked for and N otherwise. */
    void performFrequencyOnlyForwardTransform (float* inputOutputData,
                                               bool onlyCalculateNonNegativeFrequencies = false) const noexcept
    {
        const int size = getSize();

        if (size == 1)
            return;

        performRealOnlyForwardTransform (inputOutputData, onlyCalculateNonNegativeFrequencies);

        const auto* spectrum = reinterpret_cast<const std::complex<float>*> (inputOutputData);
        const int limit = onlyCalculateNonNegativeFrequencies ? (size / 2) + 1 : size;

        // Reading bin i spans floats 2i and 2i+1 while the magnitude is written
        // to float i, so every write lands at or behind the read position and
        // the aliasing is safe. This is how JUCE does it too.
        for (int i = 0; i < limit; ++i)
            inputOutputData[i] = std::abs (spectrum[i]);

        for (int i = limit; i < 2 * size; ++i)
            inputOutputData[i] = 0.0f;
    }

private:
    Fft fft;
    mutable std::vector<std::complex<float>> scratch;
};

} // namespace fxme
