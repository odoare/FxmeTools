/*
  ------------------------------------------------------------------------------
    SpectralBandSplitter.h

    Splits one mono stream into several independent frequency bands, each with
    a spectral gate, a gain and a pan, and hands back one stereo signal per
    band. Everything happens in one short-time Fourier transform: a single
    analysis FFT per hop, then one inverse FFT per active band that has
    something to output (a band whose gate is closed, or whose input is
    silent, costs no transform).

    A band is a frequency interval plus a per-bin gate: inside the interval,
    bins quieter than the gate threshold are muted and the rest pass. That is
    what makes this different from a bank of bandpass filters — the threshold
    is a horizontal line across the band's spectrum, so a band can be made to
    pass only its peaks (a tonal skeleton) or only its noise floor.

    A band can also have a ceiling, the gate's mirror: bins louder than it are
    muted. With both, a band keeps only the bins whose level lies between the
    two lines. Both switch hard by default; setGateKnee() widens each switch
    into a smooth transition a given number of dB wide, centred on its line.
    setBandDynamics() goes further, per band (see fxme::BandDynamics): under
    the gate a downward expander with a range, over the ceiling a compressor
    or limiter instead of the cut, each line with its own knee, attack and
    release. Applied bin by bin, that is a spectral expander / compressor.

    Levels use the same convention as fxme::SpectrumAnalyzer, so a gate
    threshold in dB can be drawn straight onto a SpectrumDisplay trace and mean
    what it looks like. The two only agree exactly when they run the same
    window size, though: for anything noise-like, a bin's level falls as the
    window grows (its bandwidth shrinks), so a view running a different FFT
    size than the splitter shows the same signal at a different level. Give
    both the same size, or expect the line to sit off the trace by
    10*log10(sizeRatio) dB.

    Mono or stereo input (prepare()'s numInputChannels). In stereo both
    channels are transformed and resynthesised, but every decision is taken
    on their average, the mono sum a spectrum view shows: each bin's gate and
    ceiling compare the sum's level, and the gain found is applied to both
    channels. So a band keeps its image, and a gate line drawn on the view
    of the sum means what it looks like. It costs one more forward transform
    per hop and twice the inverse ones.

    Analysis and synthesis both use a periodic Hann window with 75% overlap,
    which sums to a constant and needs no further compensation beyond the
    fixed 1/1.5 the class applies. Latency is exactly one window.

    Gain and pan are deliberately not part of the spectral stage: they are
    applied to the band's time-domain output through smoothed values, so
    moving them is click-free and costs no extra transform. Only the band
    edges and the gate touch the spectrum. A consumer that processes each
    band further before panning it (a saturator, say, which should not drive
    the louder side harder) can turn the pan off with setApplyPan (false):
    both output channels then carry the same mono band, gain applied.

    Threading: prepare() allocates — message thread / prepareToPlay only.
    setBand(), setGateTimes(), setGateKnee(), setEdgeTaperBins() and process()
    are realtime safe and expect to be called from the same (audio) thread.

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

#include <FxmeTools/dsp/BandDynamics.h>
#include <FxmeTools/util/AudioBuffer.h>
#include <FxmeTools/util/Fft.h>
#include <FxmeTools/util/Math.h>
#include <FxmeTools/util/SmoothedValue.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <memory>
#include <utility>
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

    /** Per-bin ceiling, in the same convention as the gate: bins louder than
        it are muted. Anything at or above SpectralBandSplitter::offCeilingDb
        disables it, which is the default. Last in the struct on purpose, so
        brace-initialisation written before it existed keeps its meaning. */
    float ceilingDb = 1000.0f;
};

/** Optional per-band processing inside the splitter's frame, after the band
    mask and the gate and before the inverse transform: where the FFT is
    already paid for. Installed with SpectralBandSplitter::setBandProcessor().

    Both calls come from the audio thread, from inside process(), and must be
    realtime safe. The spectrum layout is the one of fxme::RealFft: N
    interleaved complex bins, of which only the first N/2 + 1 matter (the
    inverse transform rebuilds the rest). In a band's frame, only those are
    filled in; the upper half holds nothing meaningful.

    If a band's frame reaches processBand() entirely zero and leaves it
    entirely zero, the splitter skips that band's inverse transform. */
class SpectralBandProcessor
{
public:
    virtual ~SpectralBandProcessor() = default;

    /** Once per hop, with the whole input's spectrum before any band takes
        its share. `numBins` is N/2 + 1. */
    virtual void beginFrame (const float* spectrum, int numBins) noexcept = 0;

    /** Once per hop for every enabled band, with that band's spectrum after
        its mask and gate, to be modified in place (bins 0 to numBins - 1). */
    virtual void processBand (int band, float* frame, int numBins) noexcept = 0;

    /** The stereo form, for a splitter with stereo input: both channels'
        spectra of the band. beginFrame() was given the two channels'
        average. The default processes each channel on its own with
        processBand(), which only suits a processor keeping no per-band
        state; one that does should override this. */
    virtual void processBandStereo (int band, float* left, float* right, int numBins) noexcept
    {
        processBand (band, left, numBins);
        processBand (band, right, numBins);
    }
};

class SpectralBandSplitter
{
public:
    static constexpr int minFftOrder = 8;    // 256
    static constexpr int maxFftOrder = 14;   // 16384

    /** A band edge at or below fullRangeLowHz reaches down to DC, and one at or
        above fullRangeHighHz up to Nyquist: the ends of the audible range mean
        "no limit", as for fxme::EdgeBandPass. So bands covering 20 Hz to
        20 kHz between them cover the whole spectrum, and a band touching
        either end cuts nothing there. */
    static constexpr float fullRangeLowHz  = 20.0f;
    static constexpr float fullRangeHighHz = 20000.0f;

    /** A gate at or below this is treated as fully open, and the gate stage is
        skipped entirely for that band. */
    static constexpr float openGateDb = -150.0f;

    /** A ceiling at or above this is treated as off, and skipped entirely. */
    static constexpr float offCeilingDb = 150.0f;

    /** Widest transition setGateKnee() accepts, in dB. */
    static constexpr float maxGateKneeDb = 48.0f;

    SpectralBandSplitter() = default;

    //==========================================================================
    /** Allocates everything. Message thread / prepareToPlay only.

        @param sampleRate     the stream's sample rate
        @param maxBlockSize   largest block process() will be handed
        @param numBands       how many bands to make room for
        @param fftOrder       window size exponent (11 = 2048 samples, and one
                              window is also the latency: ~43 ms at 48 kHz)
        @param numInputChannels  1 (process (mono, n)) or 2 (process (left,
                              right, n), see the class comment)
    */
    void prepare (double sampleRateIn, int maxBlockSize, int numBandsIn, int fftOrder = 11,
                  int numInputChannels = 1)
    {
        stereoInput = numInputChannels > 1;
        sampleRate = sampleRateIn > 0.0 ? sampleRateIn : 48000.0;
        order      = fxme::jlimit (minFftOrder, maxFftOrder, fftOrder);
        fftSize    = 1 << order;
        hop        = fftSize / 4;
        numBins    = fftSize / 2 + 1;
        numBands   = fxme::jmax (0, numBandsIn);
        blockSize  = fxme::jmax (1, maxBlockSize);

        fft = std::make_unique<RealFft> (order);

        window.resize ((size_t) fftSize);
        synthesisWindow.resize ((size_t) fftSize);
        for (int i = 0; i < fftSize; ++i)   // periodic Hann: sums to a constant at 75% overlap
        {
            window[(size_t) i] = 0.5f - 0.5f * std::cos (fxme::MathConstants<float>::twoPi
                                                         * (float) i / (float) fftSize);
            synthesisWindow[(size_t) i] = window[(size_t) i] * olaNorm;
        }

        history.assign ((size_t) fftSize, 0.0f);
        spectrum.assign ((size_t) (2 * fftSize), 0.0f);
        frame.assign    ((size_t) (2 * fftSize), 0.0f);

        const size_t stereoSize = stereoInput ? (size_t) (2 * fftSize) : 0;
        historyR.assign (stereoInput ? (size_t) fftSize : 0, 0.0f);
        spectrumR.assign (stereoSize, 0.0f);
        frameR.assign (stereoSize, 0.0f);
        midSpectrum.assign (stereoSize, 0.0f);

        bands.assign ((size_t) numBands, {});
        state.clear();
        state.reserve ((size_t) numBands);
        for (int b = 0; b < numBands; ++b)
        {
            BandState s;
            s.ola.assign ((size_t) (2 * fftSize), 0.0f);
            s.olaR.assign (stereoInput ? (size_t) (2 * fftSize) : 0, 0.0f);
            s.gateGain.assign ((size_t) numBins, 0.0f);
            s.ceilingGain.assign ((size_t) numBins, 1.0f);
            s.dynamics = defaultDynamics;
            state.push_back (std::move (s));
        }

        outputs.setSize (fxme::jmax (1, 2 * numBands), blockSize);
        outputs.clear();

        openness = std::make_unique<std::atomic<float>[]> ((size_t) fxme::jmax (1, numBands));
        for (int b = 0; b < numBands; ++b)
            openness[(size_t) b].store (0.0f);

        for (int b = 0; b < numBands; ++b)
            updateDynamics (b);
        setLevelSmoothingSeconds (0.02);
        reset();
    }

    /** Clears every buffer and every gate state; keeps the band settings. */
    void reset()
    {
        std::fill (history.begin(), history.end(), 0.0f);
        std::fill (historyR.begin(), historyR.end(), 0.0f);
        histPos = 0;
        hopCount = 0;
        olaRead = 0;

        for (auto& s : state)
        {
            std::fill (s.ola.begin(), s.ola.end(), 0.0f);
            std::fill (s.olaR.begin(), s.olaR.end(), 0.0f);
            std::fill (s.gateGain.begin(), s.gateGain.end(), 0.0f);
            std::fill (s.ceilingGain.begin(), s.ceilingGain.end(), 1.0f);
            s.gainL.setCurrentAndTargetValue (s.gainL.getTargetValue());
            s.gainR.setCurrentAndTargetValue (s.gainR.getTargetValue());
        }
        outputs.clear();
    }

    /** How much of band `band`'s energy its gate and ceiling let through in
        the last frame, 0 to 1: the per-bin gate gains weighted by each bin's
        energy, so a loud tone passing in a wide band reads near 1 even though
        few bins are open. 1 for a band with no level gating, 0 for a disabled
        band or a silent one. Written once per hop on the audio thread; safe
        to read from any thread (for a meter). */
    float getGateOpenness (int band) const noexcept
    {
        return openness != nullptr && fxme::isPositiveAndBelow (band, numBands)
                 ? openness[(size_t) band].load (std::memory_order_relaxed)
                 : 0.0f;
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
        if (! fxme::isPositiveAndBelow (index, numBands))
            return;

        bands[(size_t) index] = b;
        state[(size_t) index].curveDirty = true;
        updateGainTargets (index);
    }

    /** Whether the splitter pans each band (the default). Off, both output
        channels of a band carry the same mono signal with only its gain
        applied, and the band's `pan` is left for the consumer to apply,
        typically after processing the band further. Realtime safe; the
        change glides like any gain change. */
    void setApplyPan (bool shouldApplyPan) noexcept
    {
        if (applyPan == shouldApplyPan)
            return;

        applyPan = shouldApplyPan;
        for (int b = 0; b < numBands; ++b)
            updateGainTargets (b);
    }

    bool isApplyingPan() const noexcept          { return applyPan; }

    /** Installs a processor called inside each frame (see
        SpectralBandProcessor), or removes it with nullptr (the default). Not
        owned: it must outlive its installation. Call it from the thread that
        calls process(), or before processing starts. */
    void setBandProcessor (SpectralBandProcessor* newProcessor) noexcept
    {
        bandProcessor = newProcessor;
    }

    SpectralBand getBand (int index) const noexcept
    {
        return fxme::isPositiveAndBelow (index, numBands) ? bands[(size_t) index]
                                                          : SpectralBand {};
    }

    /** One band's dynamics: what its gate and ceiling do around their lines
        (expander ratio and range, compressor ratio or cut, knees, attacks
        and releases; see fxme::BandDynamics). Applied bin by bin: each bin's
        gain follows its own level, which makes this a spectral expander /
        compressor. The times are followed once per hop. Cheap; call it every
        block if convenient. */
    void setBandDynamics (int index, const BandDynamics& d) noexcept
    {
        if (! fxme::isPositiveAndBelow (index, numBands))
            return;

        state[(size_t) index].dynamics = d;
        updateDynamics (index);
    }

    /** Attack and release of both lines of every band (defaults: 5 ms and
        80 ms), as the splitter had before its dynamics were per band. */
    void setGateTimes (float attackSeconds, float releaseSeconds) noexcept
    {
        const float a = fxme::jmax (0.0f, attackSeconds), r = fxme::jmax (0.0f, releaseSeconds);
        defaultDynamics.gateAttackSeconds  = defaultDynamics.ceilingAttackSeconds  = a;
        defaultDynamics.gateReleaseSeconds = defaultDynamics.ceilingReleaseSeconds = r;
        for (int b = 0; b < numBands; ++b)
        {
            auto& d = state[(size_t) b].dynamics;
            d.gateAttackSeconds  = d.ceilingAttackSeconds  = a;
            d.gateReleaseSeconds = d.ceilingReleaseSeconds = r;
            updateDynamics (b);
        }
    }

    /** Knee of both lines of every band, in dB: 0 (the default) switches a
        bin outright, wider makes the transition that many dB wide, centred
        on the line. */
    void setGateKnee (float kneeDb) noexcept
    {
        const float knee = fxme::jlimit (0.0f, maxGateKneeDb, kneeDb);
        defaultDynamics.gateKneeDb = defaultDynamics.ceilingKneeDb = knee;
        for (int b = 0; b < numBands; ++b)
        {
            auto& d = state[(size_t) b].dynamics;
            if (d.gateKneeDb == knee && d.ceilingKneeDb == knee)
                continue;
            d.gateKneeDb = d.ceilingKneeDb = knee;
            updateDynamics (b);
        }
    }

    /** Width in bins of the raised-cosine crossfade at each band border (0
        gives a rectangular mask). It is centred on the border, half outside
        the band and half inside, so two bands sharing a border fade into each
        other with gains summing to exactly 1: touching bands with their gates
        open rebuild the input exactly, as if it had not been split. A couple
        of bins is enough to take the edge off the ringing a hard mask
        produces on a band on its own; the default is 2. */
    void setEdgeTaperBins (int bins) noexcept
    {
        edgeTaper = fxme::jlimit (0, 64, bins);
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
        assert (! stereoInput);
        processInput (mono, nullptr, numSamples);
    }

    /** The stereo form, for a splitter prepared with two input channels:
        each band's output keeps its left and right. */
    void process (const float* left, const float* right, int numSamples) noexcept
    {
        assert (stereoInput);
        processInput (left, right, numSamples);
    }

    bool isStereoInput() const noexcept          { return stereoInput; }

    /** The last process() call's output for one band. `channel` is 0 (left) or
        1 (right). Never null once prepare() has run. */
    const float* getBandOutput (int band, int channel) const noexcept
    {
        const int ch = fxme::jlimit (0, fxme::jmax (0, outputs.getNumChannels() - 1),
                                     2 * band + channel);
        return outputs.getReadPointer (ch);
    }

private:
    void processInput (const float* inL, const float* inR, int numSamples) noexcept
    {
        // A host handing over more than the block size prepare() was told about
        // would need a bigger output buffer, which cannot be allocated here.
        assert (numSamples <= outputs.getNumSamples());
        numSamples = fxme::jmin (numSamples, outputs.getNumSamples());
        if (numSamples <= 0 || numBands == 0)
            return;

        float* const* out = outputs.getArrayOfWritePointers();

        // Work in runs that stop at the next frame boundary (and, defensively,
        // at either ring's end, although both rings are multiples of the hop
        // and start aligned with it), so each band is one straight loop per run.
        for (int done = 0; done < numSamples;)
        {
            const int n = fxme::jmin (fxme::jmin (numSamples - done, hop - hopCount),
                                      fftSize - histPos, 2 * fftSize - olaRead);

            std::copy (inL + done, inL + done + n, history.data() + histPos);
            if (inR != nullptr)
                std::copy (inR + done, inR + done + n, historyR.data() + histPos);

            for (int b = 0; b < numBands; ++b)
                renderBandRun (b, out[2 * b] + done, out[2 * b + 1] + done, n);

            done     += n;
            histPos  += n;
            olaRead  += n;
            hopCount += n;

            if (histPos >= fftSize)
                histPos = 0;
            if (olaRead >= 2 * fftSize)
                olaRead = 0;

            if (hopCount >= hop)
            {
                hopCount = 0;
                renderFrame();
            }
        }
    }

    //==========================================================================
    struct BandState
    {
        std::vector<float> ola;           // overlap-add ring, 2 * fftSize
        std::vector<float> olaR;          // the right channel's, with stereo input
        std::vector<float> gateGain;      // per-bin gate gain, smoothed across frames;
                                          // 0 everywhere outside kLo..kHi
        std::vector<float> ceilingGain;   // per-bin ceiling gain, likewise; 1 outside
        int kLo = 0, kHi = -1;            // the bins the last frame covered
        BandDynamics dynamics;
        DynamicsCurve curve;
        bool curveDirty = true;           // the band's lines moved since the curve was built
        float gateAttackCoef = 0.0f, gateReleaseCoef = 0.0f;
        float ceilingAttackCoef = 0.0f, ceilingReleaseCoef = 0.0f;
        SmoothedValue<float> gainL { 0.0f }, gainR { 0.0f };
    };

    /** Sets band `index`'s two smoothed output gains from its gain and pan. */
    void updateGainTargets (int index) noexcept
    {
        const auto& b = bands[(size_t) index];
        auto& s = state[(size_t) index];
        const float g = fxme::Decibels::decibelsToGain (b.gainDb, -100.0f);

        if (! applyPan)
        {
            s.gainL.setTargetValue (g);
            s.gainR.setTargetValue (g);
            return;
        }

        // A stereo band is balanced rather than panned: the side it moves
        // away from is turned down, the other left as it is.
        if (stereoInput)
        {
            const float p = fxme::jlimit (-1.0f, 1.0f, b.pan);
            const float away = std::cos (std::abs (p) * fxme::MathConstants<float>::pi * 0.5f);
            s.gainL.setTargetValue (g * (p > 0.0f ? away : 1.0f));
            s.gainR.setTargetValue (g * (p < 0.0f ? away : 1.0f));
            return;
        }

        // Constant-power pan, so sweeping a band across the image keeps its
        // loudness; folded together with the gain into two smoothed targets.
        const float theta = (fxme::jlimit (-1.0f, 1.0f, b.pan) + 1.0f)
                                * fxme::MathConstants<float>::pi * 0.25f;
        s.gainL.setTargetValue (g * std::cos (theta));
        s.gainR.setTargetValue (g * std::sin (theta));
    }

    /** Band `index`'s time constants (in hops) from its dynamics, and its
        curve rebuilt on the next frame. */
    void updateDynamics (int index) noexcept
    {
        auto& s = state[(size_t) index];
        const auto& d = s.dynamics;

        // One update per hop, so the time constants are in hops.
        const double hopSeconds = (double) hop / sampleRate;
        s.gateAttackCoef     = coefFor (d.gateAttackSeconds,     hopSeconds);
        s.gateReleaseCoef    = coefFor (d.gateReleaseSeconds,    hopSeconds);
        s.ceilingAttackCoef  = coefFor (d.ceilingAttackSeconds,  hopSeconds);
        s.ceilingReleaseCoef = coefFor (d.ceilingReleaseSeconds, hopSeconds);
        s.curveDirty = true;
    }

    /** Rebuilds band `index`'s curve from its lines and dynamics. The
        analyser convention is level = mag * 2 / fftSize, so the raw
        magnitude a line corresponds to is the inverse of that. */
    void updateCurve (int index) noexcept
    {
        const auto& b = bands[(size_t) index];
        auto& s = state[(size_t) index];

        const auto magnitudeOf = [this] (float db) noexcept
        {
            return fxme::Decibels::decibelsToGain (db, -200.0f) * (float) fftSize * 0.5f;
        };

        s.curve.set (b.gateDb <= openGateDb ? 0.0f : magnitudeOf (b.gateDb),
                     b.ceilingDb >= offCeilingDb ? BandDynamics::infinity : magnitudeOf (b.ceilingDb),
                     s.dynamics);
        s.curveDirty = false;
    }

    static float coefFor (float seconds, double stepSeconds)
    {
        if (seconds <= 0.0f)
            return 0.0f;   // instantaneous
        return (float) std::exp (-stepSeconds / (double) seconds);
    }

    /** Band `b`'s next `n` output samples, read (and cleared) from its
        overlap-add ring at olaRead. A disabled band outputs silence, still
        draining its ring and advancing its gain glides. */
    void renderBandRun (int b, float* left, float* right, int n) noexcept
    {
        auto& s = state[(size_t) b];
        float* const v = s.ola.data() + olaRead;
        float* const vR = stereoInput ? s.olaR.data() + olaRead : nullptr;

        if (! bands[(size_t) b].enabled)
        {
            std::fill (left,  left  + n, 0.0f);
            std::fill (right, right + n, 0.0f);
            std::fill (v, v + n, 0.0f);
            if (vR != nullptr)
                std::fill (vR, vR + n, 0.0f);

            for (int i = 0; i < n && (s.gainL.isSmoothing() || s.gainR.isSmoothing()); ++i)
            {
                s.gainL.getNextValue();
                s.gainR.getNextValue();
            }
            return;
        }

        // Stereo: each side from its own ring, with its own gain.
        const float* const src = vR != nullptr ? vR : v;

        if (s.gainL.isSmoothing() || s.gainR.isSmoothing())
        {
            for (int i = 0; i < n; ++i)
            {
                left[i]  = v[i]   * s.gainL.getNextValue();
                right[i] = src[i] * s.gainR.getNextValue();
            }
        }
        else
        {
            const float gl = s.gainL.getCurrentValue();
            const float gr = s.gainR.getCurrentValue();

            for (int i = 0; i < n; ++i)
            {
                left[i]  = v[i]   * gl;
                right[i] = src[i] * gr;
            }
        }

        std::fill (v, v + n, 0.0f);
        if (vR != nullptr)
            std::fill (vR, vR + n, 0.0f);
    }

    /** One analysis frame: window and transform the last fftSize inputs, then
        for every active band mask, gate, invert and overlap-add the result.

        Only a band's own bins are computed: the frame is zeroed around them,
        and only the non-negative half is written at all, since that is all
        the inverse transform reads. A band whose frame comes out entirely
        zero (gate closed, band narrower than a bin, silent input) skips the
        inverse transform and the overlap-add, which would only add zeros. */
    void renderFrame() noexcept
    {
        // The history ring holds the last fftSize inputs; histPos is the
        // oldest, so the window starts there and wraps once.
        const auto analyse = [this] (const std::vector<float>& hist, std::vector<float>& spec)
        {
            const int tail = fftSize - histPos;
            for (int i = 0; i < tail; ++i)
                spec[(size_t) i] = hist[(size_t) (histPos + i)] * window[(size_t) i];
            for (int i = tail; i < fftSize; ++i)
                spec[(size_t) i] = hist[(size_t) (i - tail)] * window[(size_t) i];

            fft->performRealOnlyForwardTransform (spec.data(), false);
        };

        analyse (history, spectrum);

        // What every decision is taken on: the input's spectrum, or with
        // stereo input the two channels' average (the transform is linear,
        // so that is the mono sum's spectrum, at no extra transform).
        const float* detect = spectrum.data();
        if (stereoInput)
        {
            analyse (historyR, spectrumR);
            for (int i = 0; i < 2 * numBins; ++i)
                midSpectrum[(size_t) i] = 0.5f * (spectrum[(size_t) i] + spectrumR[(size_t) i]);
            detect = midSpectrum.data();
        }

        if (bandProcessor != nullptr)
            bandProcessor->beginFrame (detect, numBins);

        const float binHz = (float) (sampleRate / (double) fftSize);

        for (int b = 0; b < numBands; ++b)
        {
            const auto& cfg = bands[(size_t) b];
            if (! cfg.enabled)
            {
                openness[(size_t) b].store (0.0f, std::memory_order_relaxed);
                continue;
            }

            auto& s = state[(size_t) b];

            // The band's edges in bins, and the bins it touches: the crossfade
            // at each edge reaches half its width outside. A hard edge takes
            // the bins from its lower edge up to, not including, its upper one,
            // so two bands sharing an edge never both take the bin on it.
            // Empty (kLo > kHi) when the band falls between two bins.
            const Edges e = edgesOf (cfg, binHz);
            const int kLo = fxme::jlimit (0, numBins - 1, e.lowOpen ? 0
                                                        : (int) std::ceil (e.low - e.half));
            const int kHi = fxme::jlimit (-1, numBins - 1, e.highOpen ? numBins - 1
                                                         : e.half > 0.0f ? (int) std::floor (e.high + e.half)
                                                                         : (int) std::ceil (e.high) - 1);
            moveBandBins (s, kLo, kHi);

            float* const f = frame.data();
            const float* const x = spectrum.data();
            float* const fR = stereoInput ? frameR.data() : nullptr;
            const float* const xR = stereoInput ? spectrumR.data() : nullptr;

            const auto clearOutside = [&] (float* fr)
            {
                std::fill (fr, fr + 2 * fxme::jmin (kLo, numBins), 0.0f);
                if (kHi + 1 < numBins)
                    std::fill (fr + 2 * fxme::jmax (kLo, kHi + 1), fr + 2 * numBins, 0.0f);
            };
            clearOutside (f);
            if (fR != nullptr)
                clearOutside (fR);

            if (s.curveDirty)
                updateCurve (b);

            const bool gateOn = s.curve.isGateOn(), ceilingOn = s.curve.isCeilingOn();
            const bool levelGated = gateOn || ceilingOn;
            double passedEnergy = 0.0, bandEnergy = 0.0;
            bool anyNonZero = false;

            for (int k = kLo; k <= kHi; ++k)
            {
                const float re = detect[2 * k];
                const float im = detect[2 * k + 1];
                float gain = bandMask (k, e);
                auto& g = s.gateGain[(size_t) k];
                auto& c = s.ceilingGain[(size_t) k];

                if (levelGated)
                {
                    const float magSq = re * re + im * im;

                    // What each line asks for at this bin's level (the gate
                    // under it, the ceiling over it), each followed with its
                    // own attack and release. A ceiling cutting at or under a
                    // hard gate leaves nothing to pass.
                    const float gateTarget = gateOn ? s.curve.gateGain (magSq) : 1.0f;
                    g = gateTarget + (gateTarget > g ? s.gateAttackCoef : s.gateReleaseCoef) * (g - gateTarget);

                    const float ceilingTarget = ceilingOn ? s.curve.ceilingGain (magSq) : 1.0f;
                    // The gate acts by opening (its gain rising: its attack),
                    // the ceiling by pulling the gain down (its attack, and
                    // letting it back up its release).
                    c = ceilingTarget + (ceilingTarget < c ? s.ceilingAttackCoef : s.ceilingReleaseCoef)
                                          * (c - ceilingTarget);

                    // A release only approaches 0; land on it once inaudible,
                    // so a closed gate yields a truly silent frame (and no
                    // denormals).
                    if (g < gateFloor)
                        g = 0.0f;
                    if (c < gateFloor)
                        c = 0.0f;

                    const float passed = g * c;
                    const double weighted = (double) magSq * (double) gain;
                    bandEnergy   += weighted;
                    passedEnergy += weighted * (double) passed;

                    gain *= passed;
                }
                else
                {
                    g = 1.0f;
                    c = 1.0f;
                }

                f[2 * k]     = x[2 * k]     * gain;
                f[2 * k + 1] = x[2 * k + 1] * gain;
                anyNonZero = anyNonZero || f[2 * k] != 0.0f || f[2 * k + 1] != 0.0f;

                if (fR != nullptr)
                {
                    fR[2 * k]     = xR[2 * k]     * gain;
                    fR[2 * k + 1] = xR[2 * k + 1] * gain;
                    anyNonZero = anyNonZero || fR[2 * k] != 0.0f || fR[2 * k + 1] != 0.0f;
                }
            }

            openness[(size_t) b].store (! levelGated ? 1.0f
                                        : bandEnergy > 1.0e-12 ? (float) (passedEnergy / bandEnergy)
                                                               : 0.0f,
                                        std::memory_order_relaxed);

            if (bandProcessor != nullptr)
            {
                if (fR != nullptr)
                    bandProcessor->processBandStereo (b, f, fR, numBins);
                else
                    bandProcessor->processBand (b, f, numBins);

                // A processor can sound on its own (a frozen spectrum, a blur
                // tail), so an empty band is only skipped if it left it empty.
                const auto nonZero = [this] (const float* fr)
                {
                    return std::any_of (fr, fr + 2 * numBins, [] (float v) { return v != 0.0f; });
                };
                if (! anyNonZero)
                    anyNonZero = nonZero (f) || (fR != nullptr && nonZero (fR));
            }

            if (! anyNonZero)
                continue;

            overlapAdd (f, s.ola);
            if (fR != nullptr)
                overlapAdd (fR, s.olaR);
        }
    }

    /** Inverse transform of one band's frame, then the synthesis window and
        the overlap-add into `ola`. */
    void overlapAdd (float* f, std::vector<float>& olaRing) noexcept
    {
        fft->performRealOnlyInverseTransform (f);

        // Hann on the way out too, with the 1/1.5 the squared window sums to
        // at 75% overlap folded in. The window starts at olaRead and wraps at
        // most once.
        float* const ola = olaRing.data();
        const int first = fxme::jmin (fftSize, 2 * fftSize - olaRead);
        for (int i = 0; i < first; ++i)
            ola[olaRead + i] += f[i] * synthesisWindow[(size_t) i];
        for (int i = first; i < fftSize; ++i)
            ola[i - first] += f[i] * synthesisWindow[(size_t) i];
    }

    /** Keeps gateGain at 0 outside the band's bins when its edges move: the
        bins it leaves are cleared, so any bin entering later starts closed,
        as it would have had it been reset on every frame. */
    static void moveBandBins (BandState& s, int kLo, int kHi) noexcept
    {
        if (kLo == s.kLo && kHi == s.kHi)
            return;

        for (int k = s.kLo; k <= s.kHi; ++k)
            if (k < kLo || k > kHi)
            {
                s.gateGain[(size_t) k] = 0.0f;
                s.ceilingGain[(size_t) k] = 1.0f;
            }

        s.kLo = kLo;
        s.kHi = kHi;
    }

    /** A band's edges in bins (fractional), the crossfade's half-width, and
        whether either edge is open (at the end of the audible range). */
    struct Edges
    {
        float low = 0.0f, high = 0.0f, half = 0.0f;
        bool lowOpen = false, highOpen = false;
    };

    Edges edgesOf (const SpectralBand& b, float binHz) const noexcept
    {
        Edges e;
        e.low      = b.lowHz  / binHz;
        e.high     = b.highHz / binHz;
        e.half     = 0.5f * (float) edgeTaper;
        e.lowOpen  = b.lowHz  <= fullRangeLowHz;
        e.highOpen = b.highHz >= fullRangeHighHz;
        return e;
    }

    /** The band's gain at bin k: the step of its lower edge minus the step
        of its upper one, each step a raised cosine rising from 0 to 1 across
        its edge (centred on it), an open end being a step already at 1 (low)
        or never reached (high). Written as a difference, the gains of bands
        sharing edges add up to exactly 1 whatever their widths, even for a
        band narrower than its own crossfades. Only called for bins between
        kLo and kHi. */
    static float bandMask (int k, const Edges& e) noexcept
    {
        if (e.half <= 0.0f)
            return 1.0f;

        const float x = (float) k;
        const auto step = [&e, x] (float edge) noexcept
        {
            const float t = fxme::jlimit (0.0f, 1.0f, (x - (edge - e.half)) / (2.0f * e.half));
            return 0.5f - 0.5f * std::cos (fxme::MathConstants<float>::pi * t);
        };

        const float below = e.lowOpen  ? 1.0f : step (e.low);
        const float above = e.highOpen ? 0.0f : step (e.high);
        return fxme::jmax (0.0f, below - above);
    }

    static constexpr float olaNorm = 1.0f / 1.5f;   // sum of Hann^2 at 75% overlap

    /** A gate gain below this (-100 dB) is snapped to 0. */
    static constexpr float gateFloor = 1.0e-5f;

    std::unique_ptr<RealFft> fft;
    std::vector<float> window, synthesisWindow, history, spectrum, frame;
    std::vector<float> historyR, spectrumR, frameR, midSpectrum;   // stereo input only
    bool stereoInput = false;
    std::vector<SpectralBand> bands;
    std::vector<BandState> state;
    AudioBuffer outputs;

    double sampleRate = 48000.0;
    int order = 11, fftSize = 2048, hop = 512, numBins = 1025;
    int numBands = 0, blockSize = 512;
    int histPos = 0, hopCount = 0, olaRead = 0;
    int edgeTaper = 2;

    BandDynamics defaultDynamics;         // what a band starts with (the global setters)
    bool  applyPan = true;
    SpectralBandProcessor* bandProcessor = nullptr;
    std::unique_ptr<std::atomic<float>[]> openness;

    SpectralBandSplitter (const SpectralBandSplitter&) = delete;
    SpectralBandSplitter& operator= (const SpectralBandSplitter&) = delete;
};

} // namespace fxme
