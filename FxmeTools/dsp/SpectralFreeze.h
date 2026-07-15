/*
  ------------------------------------------------------------------------------
    SpectralFreeze.h

    FFT freeze on the WDL real FFT (WDL is the git submodule at the repo
    root, the same one the convolution engine uses): capture one window of
    audio, keep its magnitude spectrum, and resynthesize an endless static
    wash by inverse-transforming those magnitudes with fresh random phases
    every hop (paulstretch-style), Hann windows on both sides, 75%
    overlap-add. Randomising the phase each frame removes all periodicity
    (the classic smooth spectral freeze).

    All phases come from fxme::detrand keyed on (seed, tag, frame, bin), so
    a freeze is a pure function of its identity: set the same identity and
    capture the same audio, and the wash reproduces bit-exactly (frame
    counter restarts with each capture).

    Usage (one instance per channel):
        prepare();                        // allocates + self-calibrates
        setIdentity (seed, tag);          // phase stream identity
        startCapture();                   // at the freeze moment
        out = processSample (in);         // passes input through for the
                                          // first kSize samples (capture),
                                          // then crossfades into the wash
                                          // and ignores the input

    The first wash sample costs 4 inverse FFTs (overlap priming, ~tens of
    µs at 2048); afterwards it is one inverse FFT per hop. prepare()
    allocates and probes the FFT round-trip gain — message thread only;
    everything else is realtime-safe.

    NOT part of the FxmeTools module umbrella (needs the WDL sources):
        #include <FxmeTools/dsp/SpectralFreeze.h>
    and compile WDL/fft.c into the target (fxmetools_attach does, or add it
    by hand).

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <cmath>
#include <cstdint>
#include <vector>
#include <algorithm>
#include "DeterministicRandom.h"
#include "../../WDL/WDL/fft.h"

namespace fxme
{

class SpectralFreeze
{
public:
    static constexpr int kOrder = 11;
    static constexpr int kSize  = 1 << kOrder;   // 2048-sample window (~43 ms at 48 kHz)
    static constexpr int kBins  = kSize / 2;     // packed complex bins (bin 0 = DC + Nyquist)
    static constexpr int kHop   = kSize / 4;     // 75% overlap

    /** Allocates and probes the FFT round-trip gain — message thread only. */
    void prepare()
    {
        WDL_fft_init();

        window.resize ((size_t) kSize);
        for (int i = 0; i < kSize; ++i)
            window[(size_t) i] = 0.5f - 0.5f * std::cos (6.283185307179586 * i / kSize);

        capture.assign ((size_t) kSize, 0.0f);
        frameBuf.assign ((size_t) kSize, 0.0f);
        mags.assign ((size_t) kBins, 0.0f);
        ola.assign ((size_t) (2 * kSize), 0.0f);

        calibrate();
        reset();
    }

    /** Back to idle pass-through; keeps the identity. */
    void reset()
    {
        capturing = frozen = false;
        std::fill (ola.begin(), ola.end(), 0.0f);
    }

    /** Identity of the deterministic phase stream (e.g. seed + a tag built
        from lane/block/channel). Takes effect at the next startCapture(). */
    void setIdentity (uint64_t seedIn, uint64_t tagIn)
    {
        seed = seedIn;
        tag  = tagIn;
    }

    /** Begins a new freeze: the next kSize input samples are captured (and
        passed through), then the frozen wash takes over. */
    void startCapture()
    {
        reset();
        capturing = true;
        capPos    = 0;
        frame     = 0;
    }

    bool isFrozen() const { return frozen; }

    float processSample (float in)
    {
        if (capturing)
        {
            capture[(size_t) capPos] = in;
            if (++capPos == kSize)
            {
                computeSpectrum();
                primeSynthesis();
                capturing = false;
                frozen    = true;
                fadePos   = 0;
            }
            return in;
        }

        if (! frozen)
            return in;   // idle: never captured

        const float wash = ola[(size_t) olaRead];
        ola[(size_t) olaRead] = 0.0f;
        olaRead = (olaRead + 1) % (2 * kSize);

        if (++hopAcc == kHop)   // the next frame becomes audible one sample from now
        {
            hopAcc = 0;
            nextStart += kHop;
            synthesizeFrame (nextStart);
        }

        float w = 1.0f;
        if (fadePos < kHop)     // capture -> wash crossfade
            w = (float) fadePos++ / (float) kHop;
        return in + w * (wash - in);
    }

private:
    void computeSpectrum()
    {
        // WDL_real_fft expects the input pre-scaled by 0.5/len; whatever the
        // exact convention, `norm` (probed in calibrate()) makes the
        // capture -> resynthesis chain unit-gain.
        const float pre = 0.5f / (float) kSize;
        for (int i = 0; i < kSize; ++i)
            frameBuf[(size_t) i] = capture[(size_t) i] * window[(size_t) i] * pre;
        WDL_real_fft (frameBuf.data(), kSize, 0);

        const auto* spec = reinterpret_cast<const WDL_FFT_COMPLEX*> (frameBuf.data());
        mags[0] = 0.0f;   // drop DC and Nyquist (packed in bin 0)
        for (int k = 1; k < kBins; ++k)
            mags[(size_t) k] = std::sqrt (spec[k].re * spec[k].re + spec[k].im * spec[k].im);
    }

    /** One synthesis frame starting at absolute wash sample `start`:
        captured magnitudes, fresh deterministic phases, inverse FFT,
        Hann-windowed overlap-add into the ring. */
    void synthesizeFrame (int64_t start)
    {
        auto* spec = reinterpret_cast<WDL_FFT_COMPLEX*> (frameBuf.data());
        spec[0].re = spec[0].im = 0.0f;
        for (int k = 1; k < kBins; ++k)
        {
            const float theta = 6.2831853f * detrand::u01 (seed, tag, (uint64_t) frame, (uint64_t) k);
            spec[k].re = mags[(size_t) k] * std::cos (theta);
            spec[k].im = mags[(size_t) k] * std::sin (theta);
        }
        ++frame;

        WDL_real_fft (frameBuf.data(), kSize, 1);
        for (int i = 0; i < kSize; ++i)
            ola[(size_t) ((start + i) % (2 * kSize))]
                += frameBuf[(size_t) i] * window[(size_t) i] * norm;
    }

    /** Fills the overlap so the wash starts at full amplitude: the four
        frames whose windows cover the first read position. */
    void primeSynthesis()
    {
        for (int f = 0; f < 4; ++f)
            synthesizeFrame ((int64_t) f * kHop);
        nextStart = 3 * kHop;
        olaRead   = 3 * kHop;
        hopAcc    = 0;
    }

    /** Probes the forward+inverse round-trip gain with an impulse so the
        resynthesis is unit-gain regardless of the library's scaling
        convention. The 1.5 is the Hann² overlap-add sum at 75% overlap. */
    void calibrate()
    {
        std::fill (frameBuf.begin(), frameBuf.end(), 0.0f);
        frameBuf[(size_t) (kSize / 2)] = 0.5f / (float) kSize;
        WDL_real_fft (frameBuf.data(), kSize, 0);
        WDL_real_fft (frameBuf.data(), kSize, 1);
        float roundGain = frameBuf[(size_t) (kSize / 2)];
        if (! (roundGain > 1.0e-12f))
            roundGain = 1.0f;
        norm = 1.0f / (roundGain * 1.5f);
    }

    std::vector<WDL_FFT_REAL> capture, frameBuf, ola;
    std::vector<float> window, mags;

    uint64_t seed = 0, tag = 0;
    float   norm = 1.0f;
    int     capPos = 0, hopAcc = 0, olaRead = 0, fadePos = 0, frame = 0;
    int64_t nextStart = 0;
    bool    capturing = false, frozen = false;
};

} // namespace fxme
