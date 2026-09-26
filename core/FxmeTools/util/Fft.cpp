/*
  ------------------------------------------------------------------------------
    util/Fft.cpp

    RealFft's transforms, on WDL's real FFT where it can serve the order and on
    the scalar Fft elsewhere (see Fft.h for the contract both honour).

    FXME_CORE_HAS_WDL is defined for this file by core/CMakeLists.txt when the
    WDL submodule is checked out, alongside linking WDL/fft.c. It is private to
    the FxmeCore target: RealFft's layout is the same either way, so consumers
    never see it and cannot disagree about it.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/util/Fft.h>

#include <algorithm>

#if FXME_CORE_HAS_WDL
 #include "../../../WDL/WDL/fft.h"
#endif

namespace fxme
{

namespace
{
#if FXME_CORE_HAS_WDL
    // WDL can be built in double precision (WDL_FFT_REALSIZE=8). The float
    // buffers below would then be read as doubles, so refuse to compile.
    static_assert (sizeof (WDL_FFT_REAL) == sizeof (float),
                   "fxme::RealFft needs WDL's FFT in single precision (WDL_FFT_REALSIZE 4)");

    // WDL_real_fft handles 2..32768 points, but only lengths of 4 and up use
    // the permuted half-spectrum layout decoded below.
    constexpr int wdlMinOrder = 2;
    constexpr int wdlMaxOrder = 15;

    bool wdlServes (int order) noexcept
    {
        return order >= wdlMinOrder && order <= wdlMaxOrder;
    }

    /** WDL_fft_init builds global twiddle and permutation tables behind an
        unsynchronised flag; a function-local static makes the first call
        thread-safe. Idempotent after that. */
    void initWdl()
    {
        static const bool initialised = (WDL_fft_init(), true);
        (void) initialised;
    }
#else
    bool wdlServes (int) noexcept { return false; }
#endif
}

//==============================================================================
RealFft::RealFft (int fftOrder)
    : order (fftOrder < 0 ? 0 : fftOrder),
      size (1 << (fftOrder < 0 ? 0 : fftOrder)),
      scratch (static_cast<std::size_t> (size))
{
    if (! wdlServes (order))
    {
        fallback.emplace (order);
        return;
    }

#if FXME_CORE_HAS_WDL
    initWdl();
#endif
}

//==============================================================================
void RealFft::performRealOnlyForwardTransform (float* inputOutputData,
                                               bool onlyCalculateNonNegativeFrequencies) const noexcept
{
    (void) onlyCalculateNonNegativeFrequencies;

    if (size == 1)
        return;

    auto* spectrum = reinterpret_cast<std::complex<float>*> (inputOutputData);

    if (fallback)
    {
        for (int i = 0; i < size; ++i)
            scratch[static_cast<std::size_t> (i)] = { inputOutputData[i], 0.0f };

        fallback->perform (scratch.data(), spectrum, false);
        return;
    }

#if FXME_CORE_HAS_WDL
    auto* buf = reinterpret_cast<float*> (scratch.data());
    std::copy (inputOutputData, inputOutputData + size, buf);

    WDL_real_fft (buf, size, 0);

    // WDL returns N/2 complex values, bin k at index permute[k], with the DC
    // bin in [0].re and the (real) Nyquist bin in [0].im — and everything at
    // twice the unscaled DFT. Unpermute, halve, and mirror the upper half so
    // all N bins read as JUCE's would.
    const int half = size >> 1;
    const int* permute = WDL_fft_permute_tab (half);

    spectrum[0]    = { 0.5f * buf[0], 0.0f };
    spectrum[half] = { 0.5f * buf[1], 0.0f };

    for (int k = 1; k < half; ++k)
    {
        const float* bin = buf + 2 * permute[k];
        const std::complex<float> value { 0.5f * bin[0], 0.5f * bin[1] };

        spectrum[k]        = value;
        spectrum[size - k] = std::conj (value);
    }
#endif
}

void RealFft::performRealOnlyInverseTransform (float* inputOutputData) const noexcept
{
    if (size == 1)
        return;

    auto* spectrum = reinterpret_cast<std::complex<float>*> (inputOutputData);

    if (fallback)
    {
        // Mirrors the positive half onto the negative one. The DC and Nyquist
        // bins' imaginary parts only reach the imaginary output, which is
        // discarded, so the reconstruction comes out real regardless.
        for (int i = size >> 1; i < size; ++i)
            spectrum[i] = std::conj (spectrum[size - i]);

        fallback->perform (spectrum, scratch.data(), true);

        for (int i = 0; i < size; ++i)
        {
            inputOutputData[i]        = scratch[static_cast<std::size_t> (i)].real();
            inputOutputData[i + size] = scratch[static_cast<std::size_t> (i)].imag();
        }
        return;
    }

#if FXME_CORE_HAS_WDL
    // The forward layout in reverse. WDL's inverse is unscaled, so the 1/N
    // goes on at the end.
    const int half = size >> 1;
    const int* permute = WDL_fft_permute_tab (half);
    auto* buf = reinterpret_cast<float*> (scratch.data());

    buf[0] = spectrum[0].real();
    buf[1] = spectrum[half].real();

    for (int k = 1; k < half; ++k)
    {
        float* bin = buf + 2 * permute[k];
        bin[0] = spectrum[k].real();
        bin[1] = spectrum[k].imag();
    }

    WDL_real_fft (buf, size, 1);

    const float scale = 1.0f / static_cast<float> (size);

    for (int i = 0; i < size; ++i)
        inputOutputData[i] = buf[i] * scale;

    std::fill (inputOutputData + size, inputOutputData + 2 * size, 0.0f);
#endif
}

} // namespace fxme
