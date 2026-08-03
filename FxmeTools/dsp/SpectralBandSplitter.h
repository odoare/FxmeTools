/*
  ------------------------------------------------------------------------------
    SpectralBandSplitter.h

    Splits one mono stream into several independent frequency bands, each with
    a spectral gate, a gain and a pan, and hands back one stereo signal per
    band. Everything happens in one short-time Fourier transform: a single
    analysis FFT per hop, then one inverse FFT per active band.

    A band is a frequency interval plus a per-bin gate: inside the interval,
    bins quieter than the gate threshold are muted and the rest pass. That is
    what makes this different from a bank of bandpass filters — the threshold
    is a horizontal line across the band's spectrum, so a band can be made to
    pass only its peaks (a tonal skeleton) or only its noise floor.

    Levels use the same convention as fxme::SpectrumAnalyzer, so a gate
    threshold in dB can be drawn straight onto a SpectrumDisplay trace and mean
    what it looks like. The two only agree exactly when they run the same
    window size, though: for anything noise-like, a bin's level falls as the
    window grows (its bandwidth shrinks), so a view running a different FFT
    size than the splitter shows the same signal at a different level. Give
    both the same size, or expect the line to sit off the trace by
    10*log10(sizeRatio) dB.

    Analysis and synthesis both use a periodic Hann window with 75% overlap,
    which sums to a constant and needs no further compensation beyond the
    fixed 1/1.5 the class applies. Latency is exactly one window.

    Gain and pan are deliberately not part of the spectral stage: they are
    applied to the band's time-domain output through smoothed values, so
    moving them is click-free and costs no extra transform. Only the band
    edges and the gate touch the spectrum.

    Threading: prepare() allocates — message thread / prepareToPlay only.
    setBand(), setGateTimes(), setEdgeTaperBins() and process() are realtime
    safe and expect to be called from the same (audio) thread.

    Usage:

        splitter.prepare (sampleRate, samplesPerBlock, numBands, 11);
        setLatencySamples (splitter.getLatencySamples());
        ...
        for (int b = 0; b < numBands; ++b)
            splitter.setBand (b, { true, 200.0f, 2000.0f, -60.0f, 0.0f, -0.5f });

        splitter.process (monoInput, numSamples);
        const float* left  = splitter.getBandOutput (b, 0);
        const float* right = splitter.getBandOutput (b, 1);

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>
#include <cmath>
#include <vector>

namespace fxme
{

/** One band's settings. Passed by value to SpectralBandSplitter::setBand(). */
struct SpectralBand
{
    bool  enabled = false;
    float lowHz   = 20.0f;
    float highHz  = 20000.0f;

    /** Per-bin gate threshold, in the dB convention of fxme::SpectrumAnalyzer.
        Anything at or below SpectralBandSplitter::openGateDb leaves the band
        wide open (no bin is ever rejected), which is the sensible default. */
    float gateDb  = -1000.0f;

    float gainDb  = 0.0f;
    float pan     = 0.0f;      // -1 = hard left, +1 = hard right, constant power
};

class SpectralBandSplitter
{
public:
    static constexpr int minFftOrder = 8;    // 256
    static constexpr int maxFftOrder = 14;   // 16384

    /** A gate at or below this is treated as fully open, and the gate stage is
        skipped entirely for that band. */
    static constexpr float openGateDb = -150.0f;

    SpectralBandSplitter() = default;

    //==========================================================================
    /** Allocates everything. Message thread / prepareToPlay only.

        @param sampleRate     the stream's sample rate
        @param maxBlockSize   largest block process() will be handed
        @param numBands       how many bands to make room for
        @param fftOrder       window size exponent (11 = 2048 samples, and one
                              window is also the latency: ~43 ms at 48 kHz)
    */
    void prepare (double sampleRateIn, int maxBlockSize, int numBandsIn, int fftOrder = 11)
    {
        sampleRate = sampleRateIn > 0.0 ? sampleRateIn : 48000.0;
        order      = juce::jlimit (minFftOrder, maxFftOrder, fftOrder);
        fftSize    = 1 << order;
        hop        = fftSize / 4;
        numBins    = fftSize / 2 + 1;
        numBands   = juce::jmax (0, numBandsIn);
        blockSize  = juce::jmax (1, maxBlockSize);

        fft = std::make_unique<juce::dsp::FFT> (order);

        window.resize ((size_t) fftSize);
        for (int i = 0; i < fftSize; ++i)   // periodic Hann: sums to a constant at 75% overlap
            window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi
                                                         * (float) i / (float) fftSize);

        history.assign ((size_t) fftSize, 0.0f);
        spectrum.assign ((size_t) (2 * fftSize), 0.0f);
        frame.assign    ((size_t) (2 * fftSize), 0.0f);

        bands.assign ((size_t) numBands, {});
        state.clear();
        state.reserve ((size_t) numBands);
        for (int b = 0; b < numBands; ++b)
        {
            BandState s;
            s.ola.assign ((size_t) (2 * fftSize), 0.0f);
            s.gateGain.assign ((size_t) numBins, 0.0f);
            state.push_back (std::move (s));
        }

        outputs.setSize (juce::jmax (1, 2 * numBands), blockSize);
        outputs.clear();

        setGateTimes (gateAttackSeconds, gateReleaseSeconds);
        setLevelSmoothingSeconds (0.02);
        reset();
    }

    /** Clears every buffer and every gate state; keeps the band settings. */
    void reset()
    {
        std::fill (history.begin(), history.end(), 0.0f);
        histPos = 0;
        hopCount = 0;
        olaRead = 0;

        for (auto& s : state)
        {
            std::fill (s.ola.begin(), s.ola.end(), 0.0f);
            std::fill (s.gateGain.begin(), s.gateGain.end(), 0.0f);
            s.gainL.setCurrentAndTargetValue (s.gainL.getTargetValue());
            s.gainR.setCurrentAndTargetValue (s.gainR.getTargetValue());
        }
        outputs.clear();
    }

    int    getNumBands() const noexcept        { return numBands; }
    int    getFftSize() const noexcept         { return fftSize; }
    int    getHopSize() const noexcept         { return hop; }
    double getSampleRate() const noexcept      { return sampleRate; }

    /** Delay the splitter adds, in samples: exactly one analysis window. Report
        it to the host with AudioProcessor::setLatencySamples(). */
    int getLatencySamples() const noexcept     { return fftSize; }

    //==========================================================================
    /** Updates one band. Cheap: only the gate threshold is turned into a
        comparable magnitude, and gain/pan become smoothed targets. */
    void setBand (int index, const SpectralBand& b) noexcept
    {
        if (! juce::isPositiveAndBelow (index, numBands))
            return;

        bands[(size_t) index] = b;
        auto& s = state[(size_t) index];

        // Compare squared magnitudes so the gate costs no logarithm per bin.
        // The analyser convention is level = mag * 2 / fftSize, so the raw
        // magnitude a threshold corresponds to is the inverse of that.
        const float thresholdMag = juce::Decibels::decibelsToGain (b.gateDb, -200.0f)
                                     * (float) fftSize * 0.5f;
        s.gateThresholdSq = thresholdMag * thresholdMag;
        s.gateOpen = b.gateDb <= openGateDb;

        // Constant-power pan, so sweeping a band across the image keeps its
        // loudness; folded together with the gain into two smoothed targets.
        const float g     = juce::Decibels::decibelsToGain (b.gainDb, -100.0f);
        const float theta = (juce::jlimit (-1.0f, 1.0f, b.pan) + 1.0f)
                                * juce::MathConstants<float>::pi * 0.25f;
        s.gainL.setTargetValue (g * std::cos (theta));
        s.gainR.setTargetValue (g * std::sin (theta));
    }

    SpectralBand getBand (int index) const noexcept
    {
        return juce::isPositiveAndBelow (index, numBands) ? bands[(size_t) index]
                                                          : SpectralBand {};
    }

    /** How fast a bin opens once it rises above the threshold, and how fast it
        closes again. Smoothing the per-bin gate across frames is what keeps a
        spectral gate from warbling; the release is deliberately the slower of
        the two (defaults: 5 ms and 80 ms). */
    void setGateTimes (float attackSeconds, float releaseSeconds) noexcept
    {
        gateAttackSeconds  = juce::jmax (0.0f, attackSeconds);
        gateReleaseSeconds = juce::jmax (0.0f, releaseSeconds);

        // One update per hop, so the time constants are in hops.
        const double hopSeconds = (double) hop / sampleRate;
        gateAttackCoef  = coefFor (gateAttackSeconds,  hopSeconds);
        gateReleaseCoef = coefFor (gateReleaseSeconds, hopSeconds);
    }

    /** Width in bins of the raised-cosine taper at each band border (0 gives a
        rectangular mask). A couple of bins is enough to take the edge off the
        ringing a hard mask produces; the default is 2. */
    void setEdgeTaperBins (int bins) noexcept
    {
        edgeTaper = juce::jlimit (0, 64, bins);
    }

    /** Glide applied to gain and pan changes, in seconds (default 20 ms). */
    void setLevelSmoothingSeconds (double seconds)
    {
        for (auto& s : state)
        {
            s.gainL.reset (sampleRate, seconds);
            s.gainR.reset (sampleRate, seconds);
        }
    }

    //==========================================================================
    /** Feeds `numSamples` mono samples in and renders the same number of
        samples of every band's stereo output. Disabled bands are silenced (and
        cost nothing beyond that). */
    void process (const float* mono, int numSamples) noexcept
    {
        // A host handing over more than the block size prepare() was told about
        // would need a bigger output buffer, which cannot be allocated here.
        jassert (numSamples <= outputs.getNumSamples());
        numSamples = juce::jmin (numSamples, outputs.getNumSamples());
        if (numSamples <= 0 || numBands == 0)
            return;

        float* const* out = outputs.getArrayOfWritePointers();

        for (int i = 0; i < numSamples; ++i)
        {
            history[(size_t) histPos] = mono[i];
            if (++histPos >= fftSize)
                histPos = 0;

            for (int b = 0; b < numBands; ++b)
            {
                auto& s = state[(size_t) b];
                const float v = s.ola[(size_t) olaRead];
                s.ola[(size_t) olaRead] = 0.0f;

                const float gl = s.gainL.getNextValue();
                const float gr = s.gainR.getNextValue();
                const bool  on = bands[(size_t) b].enabled;

                out[2 * b][i]     = on ? v * gl : 0.0f;
                out[2 * b + 1][i] = on ? v * gr : 0.0f;
            }

            if (++olaRead >= 2 * fftSize)
                olaRead = 0;

            if (++hopCount >= hop)
            {
                hopCount = 0;
                renderFrame();
            }
        }
    }

    /** The last process() call's output for one band. `channel` is 0 (left) or
        1 (right). Never null once prepare() has run. */
    const float* getBandOutput (int band, int channel) const noexcept
    {
        const int ch = juce::jlimit (0, juce::jmax (0, outputs.getNumChannels() - 1),
                                     2 * band + channel);
        return outputs.getReadPointer (ch);
    }

private:
    //==========================================================================
    struct BandState
    {
        std::vector<float> ola;           // overlap-add ring, 2 * fftSize
        std::vector<float> gateGain;      // per-bin gate gain, smoothed across frames
        float gateThresholdSq = 0.0f;
        bool  gateOpen = true;
        juce::SmoothedValue<float> gainL { 0.0f }, gainR { 0.0f };
    };

    static float coefFor (float seconds, double stepSeconds)
    {
        if (seconds <= 0.0f)
            return 0.0f;   // instantaneous
        return (float) std::exp (-stepSeconds / (double) seconds);
    }

    /** One analysis frame: window and transform the last fftSize inputs, then
        for every active band mask, gate, invert and overlap-add the result. */
    void renderFrame() noexcept
    {
        // The history ring holds the last fftSize inputs; histPos is the oldest.
        for (int i = 0; i < fftSize; ++i)
        {
            const int r = (histPos + i) % fftSize;
            spectrum[(size_t) i] = history[(size_t) r] * window[(size_t) i];
        }
        std::fill (spectrum.begin() + fftSize, spectrum.end(), 0.0f);
        fft->performRealOnlyForwardTransform (spectrum.data(), false);

        const float binHz = (float) (sampleRate / (double) fftSize);

        for (int b = 0; b < numBands; ++b)
        {
            const auto& cfg = bands[(size_t) b];
            if (! cfg.enabled)
                continue;

            auto& s = state[(size_t) b];

            // Bins fully inside the band, plus the taper skirt on each side.
            const int kLo = juce::jlimit (0, numBins - 1, (int) std::ceil  (cfg.lowHz  / binHz));
            const int kHi = juce::jlimit (0, numBins - 1, (int) std::floor (cfg.highHz / binHz));

            std::copy (spectrum.begin(), spectrum.end(), frame.begin());

            for (int k = 0; k < numBins; ++k)
            {
                float gain = bandMask (k, kLo, kHi);

                if (gain > 0.0f && ! s.gateOpen)
                {
                    const float re = frame[(size_t) (2 * k)];
                    const float im = frame[(size_t) (2 * k + 1)];
                    const float magSq = re * re + im * im;

                    const float target = magSq >= s.gateThresholdSq ? 1.0f : 0.0f;
                    const float coef   = target > s.gateGain[(size_t) k] ? gateAttackCoef
                                                                         : gateReleaseCoef;
                    auto& g = s.gateGain[(size_t) k];
                    g = target + coef * (g - target);
                    gain *= g;
                }
                else if (gain > 0.0f)
                {
                    s.gateGain[(size_t) k] = 1.0f;
                }
                else
                {
                    s.gateGain[(size_t) k] = 0.0f;
                }

                scaleBin (frame.data(), k, gain);
            }

            fft->performRealOnlyInverseTransform (frame.data());

            // Hann on the way out too, and the 1/1.5 the squared window sums to
            // at 75% overlap.
            int w = olaRead;
            for (int i = 0; i < fftSize; ++i)
            {
                s.ola[(size_t) w] += frame[(size_t) i] * window[(size_t) i] * olaNorm;
                if (++w >= 2 * fftSize)
                    w = 0;
            }
        }
    }

    /** 1 inside the band, 0 outside, raised cosine across the taper skirt. */
    float bandMask (int k, int kLo, int kHi) const noexcept
    {
        if (k < kLo || k > kHi)
            return 0.0f;
        if (edgeTaper <= 0)
            return 1.0f;

        const int d = juce::jmin (k - kLo, kHi - k);
        if (d >= edgeTaper)
            return 1.0f;

        const float x = (float) (d + 1) / (float) (edgeTaper + 1);
        return 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * x);
    }

    /** Scales bin k of a real-only transform, mirroring onto its conjugate so
        the inverse transform stays real. */
    void scaleBin (float* data, int k, float gain) const noexcept
    {
        data[2 * k]     *= gain;
        data[2 * k + 1] *= gain;

        // Bin 0 (DC) and bin fftSize/2 (Nyquist) are their own mirror image and
        // must not be scaled twice.
        const int mirror = fftSize - k;
        if (k > 0 && mirror != k && mirror < fftSize)
        {
            data[2 * mirror]     *= gain;
            data[2 * mirror + 1] *= gain;
        }
    }

    static constexpr float olaNorm = 1.0f / 1.5f;   // sum of Hann^2 at 75% overlap

    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window, history, spectrum, frame;
    std::vector<SpectralBand> bands;
    std::vector<BandState> state;
    juce::AudioBuffer<float> outputs;

    double sampleRate = 48000.0;
    int order = 11, fftSize = 2048, hop = 512, numBins = 1025;
    int numBands = 0, blockSize = 512;
    int histPos = 0, hopCount = 0, olaRead = 0;
    int edgeTaper = 2;

    float gateAttackSeconds = 0.005f, gateReleaseSeconds = 0.080f;
    float gateAttackCoef = 0.0f, gateReleaseCoef = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectralBandSplitter)
};

} // namespace fxme
