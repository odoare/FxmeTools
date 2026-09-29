/*
  ------------------------------------------------------------------------------
    SpectralBandEffects.h

    Pitch shift, freeze and blur for the bands of a fxme::SpectralBandSplitter,
    done inside the splitter's own frames (a SpectralBandProcessor), so they
    cost no extra transform and no extra latency.

    All three rest on one phase vocoder. Once per hop, the whole input's
    spectrum is analysed: each bin's phase, and from its advance since the last
    hop, the true frequency of whatever the bin holds. Every band shares that
    analysis. Then, per band and per effect:

      - Pitch: bin k's magnitude moves to bin round (k * ratio), with its
        frequency scaled by the ratio. Any ratio works, so semitones can be
        fractional. Scaling bin positions (not sliding them by a fixed number)
        keeps harmonics harmonic: this is a pitch shift, not a frequency shift.
      - Freeze: the band's magnitudes, frequencies and phase pattern are
        captured on the first frame it is on, and resynthesised from then on,
        ignoring the input. Two modes: tonal (phases advance at the captured
        frequencies, so held notes stay notes) and wash (a fresh random phase
        per bin and per hop, paulstretch-style, a smooth static texture; the
        phase stream is a pure function of band, frame and bin through
        fxme::detrand, so a freeze reproduces exactly).
      - Blur: each bin's magnitude is smoothed across hops with a one-pole of
        a given time constant, on top of the tracked frequencies: a reverb-like
        tail confined to the band rather than a smear.

    Order per band: freeze or blur (magnitudes), then pitch (remapping), then
    synthesis. Freeze wins over blur when both are on (a frozen band is steady
    already).

    Synthesis uses identity phase locking (Laroche and Dolson): only spectral
    peaks get their phase advanced from their frequency; the bins around each
    peak keep their original phase offsets to it. Without it, a phase vocoder
    sounds phasey (every bin drifting on its own); with it, partials stay
    coherent. Transients still soften, as with any phase vocoder, more so at
    the splitter's 75% overlap, which is the least a phase vocoder can use.

    A band with every effect off is left untouched, bit for bit, so installing
    this processor changes nothing until an effect is switched on.

    Threading: prepare() allocates (message thread / prepareToPlay only);
    setBand(), reset() and the SpectralBandProcessor calls are realtime safe
    and belong to the audio thread.

    Usage:

        splitter.prepare (sampleRate, blockSize, numBands, order);
        effects.prepare (splitter.getFftSize(), splitter.getHopSize(),
                         sampleRate, numBands);
        splitter.setBandProcessor (&effects);
        ...
        fxme::SpectralBandEffects::Settings s;
        s.pitchOn = true;
        s.pitchSemitones = 7.0f;
        effects.setBand (band, s);            // every block is fine

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/dsp/SpectralBandSplitter.h>
#include <FxmeTools/dsp/DeterministicRandom.h>
#include <FxmeTools/util/Math.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace fxme
{

class SpectralBandEffects : public SpectralBandProcessor
{
public:
    enum class FreezeMode { tonal = 0, wash };

    /** One band's effects. Passed by value to setBand(). */
    struct Settings
    {
        bool  pitchOn = false;
        float pitchSemitones = 0.0f;

        bool       freezeOn = false;
        FreezeMode freezeMode = FreezeMode::tonal;

        bool  blurOn = false;
        float blurSeconds = 0.3f;
    };

    /** Allocates for bands of a splitter running `fftSize` points with hops of
        `hop` samples. Message thread / prepareToPlay only. */
    void prepare (int fftSize, int hopSize, double sampleRateIn, int numBandsIn)
    {
        size       = std::max (4, fftSize);
        hop        = std::max (1, hopSize);
        numBins    = size / 2 + 1;
        sampleRate = sampleRateIn > 0.0 ? sampleRateIn : 48000.0;
        numBands   = std::max (0, numBandsIn);

        // Expected phase advance per bin per hop, in radians per bin index.
        expectedAdvance = twoPi * (double) hop / (double) size;

        // A wash has random phases from hop to hop, so its overlapping frames
        // add as powers rather than amplitudes: it comes out quieter than the
        // same magnitudes resynthesised coherently. For the splitter's Hann
        // analysis and synthesis windows and its 1/1.5 overlap-add scaling,
        // the level is restored by sqrt (1.5 N / sum (w^2)), which is 2 at
        // 75% overlap. Computed from the window rather than hard-coded.
        double sumW2 = 0.0;
        for (int i = 0; i < size; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos (twoPi * (double) i / (double) size);
            sumW2 += w * w;
        }
        washGain = (float) std::sqrt (1.5 * (double) size / sumW2);

        const auto bins = (size_t) numBins;
        analysisPhase.assign (bins, 0.0f);
        previousPhase.assign (bins, 0.0f);
        trueFrequency.assign (bins, 0.0f);
        magnitude.assign (bins, 0.0f);
        magnitudeL.assign (bins, 0.0f);
        magnitudeR.assign (bins, 0.0f);
        deltaL.assign (bins, 0.0f);
        deltaR.assign (bins, 0.0f);
        outMagnitude.assign (bins, 0.0f);
        outMagnitudeR.assign (bins, 0.0f);
        outDeltaL.assign (bins, 0.0f);
        outDeltaR.assign (bins, 0.0f);
        outPhase.assign (bins, 0.0f);
        strongest.assign (bins, 0.0f);
        peaks.assign (bins, 0);

        settings.assign ((size_t) numBands, {});
        state.clear();
        state.resize ((size_t) numBands);
        for (auto& s : state)
        {
            s.frozenMagnitude.assign (bins, 0.0f);
            s.frozenFrequency.assign (bins, 0.0f);
            s.frozenPhase.assign (bins, 0.0f);
            s.blurredMagnitude.assign (bins, 0.0f);
            s.blurredFrequency.assign (bins, 0.0f);
            s.synthesisPhase.assign (bins, 0.0f);
            s.frozenMagnitudeR.assign (bins, 0.0f);
            s.frozenDeltaL.assign (bins, 0.0f);
            s.frozenDeltaR.assign (bins, 0.0f);
            s.blurredMagnitudeR.assign (bins, 0.0f);
            s.blurredDeltaL.assign (bins, 0.0f);
            s.blurredDeltaR.assign (bins, 0.0f);
        }

        reset();
    }

    /** Forgets every capture, smoothing and phase history; keeps settings. */
    void reset() noexcept
    {
        analysedLastHop = false;
        frequenciesTracked = false;
        frameCounter = 0;
        for (auto& s : state)
        {
            s.wasActive = false;
            s.frozen = false;
            s.blurPrimed = false;
        }
    }

    /** Forgets one band's capture, smoothing and phase history, for a band
        whose slot is being reused: it starts as if its effects were just
        switched on. Realtime safe. */
    void resetBand (int band) noexcept
    {
        if (band < 0 || band >= numBands)
            return;

        auto& s = state[(size_t) band];
        s.wasActive = false;
        s.frozen = false;
        s.blurPrimed = false;
    }

    /** Updates one band. Cheap; call it every block if convenient. */
    void setBand (int band, const Settings& newSettings) noexcept
    {
        if (band >= 0 && band < numBands)
            settings[(size_t) band] = newSettings;
    }

    /** True when any effect of `band` is on (the band is then resynthesised). */
    bool isBandActive (int band) const noexcept
    {
        return band >= 0 && band < numBands && isActive (settings[(size_t) band]);
    }

    //==========================================================================
    void beginFrame (const float* spectrum, int bins) noexcept override
    {
        ++frameCounter;

        bool anyActive = false;
        for (const auto& s : settings)
            anyActive = anyActive || isActive (s);

        // Nothing to do this hop. The phase history goes stale, so the next
        // analysis starts from bin-centre frequencies instead of trusting it.
        if (! anyActive || bins != numBins)
        {
            analysedLastHop = false;
            frequenciesTracked = false;
            return;
        }

        // This hop's frequencies are only real if the previous hop was
        // analysed too; otherwise they are bin centres, which a tonal freeze
        // captured now would hold up to half a bin out of tune.
        frequenciesTracked = analysedLastHop;

        for (int k = 0; k < numBins; ++k)
        {
            const float re = spectrum[2 * k];
            const float im = spectrum[2 * k + 1];
            const float phase = std::atan2 (im, re);

            // The bin's true frequency, in bins: its centre plus whatever
            // deviation explains the phase advance beyond the expected one.
            float frequency = (float) k;
            if (analysedLastHop)
            {
                const double deviation = wrap ((double) phase - (double) previousPhase[(size_t) k]
                                               - expectedAdvance * (double) k);
                frequency = (float) ((double) k + deviation / expectedAdvance);
            }

            analysisPhase[(size_t) k] = phase;
            previousPhase[(size_t) k] = phase;
            trueFrequency[(size_t) k] = frequency;
        }

        analysedLastHop = true;
    }

    void processBand (int band, float* frame, int bins) noexcept override
    {
        processBandImpl<false> (band, frame, nullptr, bins);
    }

    /** Stereo: the analysis (peaks, frequencies, the phase each peak
        carries on with) is the mono sum's, from beginFrame(); each channel
        is resynthesised with its own magnitudes and its own phase offset to
        the sum, bin by bin. So both channels move together, as one image,
        and a bin panned one way stays panned that way. Content that cancels
        in the sum (a pure side signal) has no reliable phase of its own
        there: it takes the left channel as its reference instead. */
    void processBandStereo (int band, float* left, float* right, int bins) noexcept override
    {
        processBandImpl<true> (band, left, right, bins);
    }

private:
    template <bool stereo>
    void processBandImpl (int band, float* frame, float* right, int bins) noexcept
    {
        if (band < 0 || band >= numBands || bins != numBins)
            return;
        (void) right;

        const auto& cfg = settings[(size_t) band];
        auto& s = state[(size_t) band];

        // Until the frequencies are tracked (the first hop after the effects
        // wake up), the band passes through untouched: one hop, ~11 ms.
        if (! isActive (cfg) || ! frequenciesTracked)
        {
            s.wasActive = false;
            s.frozen = false;
            s.blurPrimed = false;
            return;
        }

        if constexpr (stereo)
            analyseStereo (frame, right);
        else
            for (int k = 0; k < numBins; ++k)
                magnitude[(size_t) k] = std::hypot (frame[2 * k], frame[2 * k + 1]);

        // ---- 1. What to resynthesise: frozen, blurred or live -----------------
        // In stereo, srcMagnitude is the left channel's and `src` holds the
        // right's and both phase offsets to the sum.
        StereoSource src { magnitudeR.data(), deltaL.data(), deltaR.data() };
        const float* srcMagnitude = stereo ? magnitudeL.data() : magnitude.data();
        const float* srcFrequency = trueFrequency.data();
        const float* srcPhase     = analysisPhase.data();

        if (cfg.freezeOn)
        {
            if (! s.frozen)
            {
                std::copy (magnitude.begin(),     magnitude.end(),     s.frozenMagnitude.begin());
                std::copy (trueFrequency.begin(), trueFrequency.end(), s.frozenFrequency.begin());
                std::copy (analysisPhase.begin(), analysisPhase.end(), s.frozenPhase.begin());
                if constexpr (stereo)
                {
                    std::copy (magnitudeL.begin(), magnitudeL.end(), s.frozenMagnitude.begin());
                    std::copy (magnitudeR.begin(), magnitudeR.end(), s.frozenMagnitudeR.begin());
                    std::copy (deltaL.begin(), deltaL.end(), s.frozenDeltaL.begin());
                    std::copy (deltaR.begin(), deltaR.end(), s.frozenDeltaR.begin());
                }
                s.frozen = true;
            }

            srcMagnitude = s.frozenMagnitude.data();
            srcFrequency = s.frozenFrequency.data();
            srcPhase     = s.frozenPhase.data();
            if constexpr (stereo)
            {
                src.magR   = s.frozenMagnitudeR.data();
                src.deltaL = s.frozenDeltaL.data();
                src.deltaR = s.frozenDeltaR.data();
            }
        }
        else
        {
            s.frozen = false;
        }

        if (cfg.blurOn && ! cfg.freezeOn)
        {
            if (! s.blurPrimed)
            {
                std::copy (magnitude.begin(),     magnitude.end(),     s.blurredMagnitude.begin());
                std::copy (trueFrequency.begin(), trueFrequency.end(), s.blurredFrequency.begin());
                if constexpr (stereo)
                {
                    std::copy (magnitudeL.begin(), magnitudeL.end(), s.blurredMagnitude.begin());
                    std::copy (magnitudeR.begin(), magnitudeR.end(), s.blurredMagnitudeR.begin());
                    std::copy (deltaL.begin(), deltaL.end(), s.blurredDeltaL.begin());
                    std::copy (deltaR.begin(), deltaR.end(), s.blurredDeltaR.begin());
                }
                s.blurPrimed = true;
            }
            else
            {
                const double hopSeconds = (double) hop / sampleRate;
                const float a = cfg.blurSeconds > 0.0f
                                  ? (float) (1.0 - std::exp (-hopSeconds / (double) cfg.blurSeconds))
                                  : 1.0f;
                for (int k = 0; k < numBins; ++k)
                {
                    const auto i = (size_t) k;

                    // In stereo each channel's magnitude is blurred on its own,
                    // and the reliability test is on their sum.
                    float blurredSum;
                    if constexpr (stereo)
                    {
                        s.blurredMagnitude[i]  += a * (magnitudeL[i] - s.blurredMagnitude[i]);
                        s.blurredMagnitudeR[i] += a * (magnitudeR[i] - s.blurredMagnitudeR[i]);
                        blurredSum = s.blurredMagnitude[i] + s.blurredMagnitudeR[i];
                    }
                    else
                    {
                        auto& m = s.blurredMagnitude[i];
                        m += a * (magnitude[i] - m);
                        blurredSum = m;
                    }

                    // A bin's tracked frequency is only trusted while the live
                    // signal still holds a fair share of the blurred level. In
                    // a tail the analysis sees silence and its frequencies are
                    // noise, so the last reliable one is held (and in stereo,
                    // the last reliable phase offsets between the channels).
                    if (magnitude[i] >= 0.25f * blurredSum)
                    {
                        s.blurredFrequency[i] = trueFrequency[i];
                        if constexpr (stereo)
                        {
                            s.blurredDeltaL[i] = deltaL[i];
                            s.blurredDeltaR[i] = deltaR[i];
                        }
                    }
                }
            }

            srcMagnitude = s.blurredMagnitude.data();
            srcFrequency = s.blurredFrequency.data();
            if constexpr (stereo)
            {
                src.magR   = s.blurredMagnitudeR.data();
                src.deltaL = s.blurredDeltaL.data();
                src.deltaR = s.blurredDeltaR.data();
            }

            // In a tail the analysis phases are those of silence, useless as a
            // pattern around a peak: use the one a steady partial has instead.
            srcPhase = nullptr;
        }
        else
        {
            s.blurPrimed = false;
        }

        // ---- 2 and 3. Pitch and synthesis -------------------------------------
        const float ratio = cfg.pitchOn ? std::exp2 (cfg.pitchSemitones / 12.0f) : 1.0f;
        const bool wash = cfg.freezeOn && cfg.freezeMode == FreezeMode::wash;

        resynthesise<stereo> (band, s, frame, right, srcMagnitude, src, srcFrequency, srcPhase,
                              std::abs (ratio - 1.0f) > 1.0e-5f ? ratio : 1.0f, wash);

        s.wasActive = true;
    }

    static constexpr double twoPi = 6.283185307179586476925;
    static constexpr double pi    = 3.141592653589793238462;

    struct BandState
    {
        std::vector<float> frozenMagnitude, frozenFrequency, frozenPhase;
        std::vector<float> blurredMagnitude, blurredFrequency;
        std::vector<float> synthesisPhase;

        // Stereo only: the right channel's magnitudes and both channels'
        // phase offsets to the sum, frozen or blurred like the rest (the
        // left channel's magnitudes use frozenMagnitude / blurredMagnitude).
        std::vector<float> frozenMagnitudeR, frozenDeltaL, frozenDeltaR;
        std::vector<float> blurredMagnitudeR, blurredDeltaL, blurredDeltaR;

        bool wasActive = false;
        bool frozen = false;
        bool blurPrimed = false;
    };

    /** In stereo, what goes with the left channel's magnitudes: the right
        channel's, and each channel's phase offset to the sum. */
    struct StereoSource
    {
        const float* magR;
        const float* deltaL;
        const float* deltaR;
    };

    /** Stereo analysis of one band: each channel's magnitude, their sum (in
        `magnitude`, what peaks are found on), and each channel's phase
        offset to the sum's phase (analysisPhase, from beginFrame()). Where
        the sum cancels, its phase means nothing: the left channel is then
        the reference (offset 0) and the right keeps its offset to it. */
    void analyseStereo (const float* left, const float* right) noexcept
    {
        for (int k = 0; k < numBins; ++k)
        {
            const auto i = (size_t) k;
            const float lr = left[2 * k],  li = left[2 * k + 1];
            const float rr = right[2 * k], ri = right[2 * k + 1];

            const float mL = std::hypot (lr, li);
            const float mR = std::hypot (rr, ri);
            magnitudeL[i] = mL;
            magnitudeR[i] = mR;
            magnitude[i]  = mL + mR;

            const float phaseL = std::atan2 (li, lr);
            const float phaseR = std::atan2 (ri, rr);
            const float mid = std::hypot (lr + rr, li + ri);

            if (mid > 0.05f * (mL + mR))
            {
                deltaL[i] = (float) wrap ((double) phaseL - (double) analysisPhase[i]);
                deltaR[i] = (float) wrap ((double) phaseR - (double) analysisPhase[i]);
            }
            else
            {
                deltaL[i] = 0.0f;
                deltaR[i] = (float) wrap ((double) phaseR - (double) phaseL);
            }
        }
    }

    static bool isActive (const Settings& s) noexcept
    {
        return s.freezeOn || s.blurOn
            || (s.pitchOn && std::abs (s.pitchSemitones) > 1.0e-4f);
    }

    /** Wraps an angle into (-pi, pi]. */
    static double wrap (double x) noexcept
    {
        return x - twoPi * std::floor (x / twoPi + 0.5);
    }

    /** Peak-locked resynthesis, with the pitch shift done the way Laroche and
        Dolson do it in a phase vocoder: each spectral peak and the bins around
        it (up to the midpoint with the next peak) move together, by the one
        whole number of bins that brings the peak nearest its new frequency.
        The lobe keeps its shape, so the level holds, and the exact new
        frequency comes from the peak's phase advance, not its bin.

        The peak's phase is where it was one hop ago (at its new bin) plus one
        hop at its new frequency; the bins around it keep their offset to it
        (identity phase locking). The offsets come from `phase`, the analysis
        phases, or, when null, from the pattern a steady partial has in this
        window: neighbouring bins pi apart.

        `wash` replaces every phase by a deterministic random one instead, at
        the level a coherent resynthesis would have. */
    template <bool stereo>
    void resynthesise (int band, BandState& s, float* frame, float* right,
                       const float* mag, const StereoSource& src,
                       const float* freq, const float* phase,
                       float ratio, bool wash) noexcept
    {
        // What peaks are found on: the band's magnitude, or in stereo the
        // sum of both channels' (so content on one side only still counts).
        const auto level = [&] (int k) noexcept
        {
            if constexpr (stereo)
                return mag[k] + src.magR[k];
            else
                return mag[k];
        };

        int numPeaks = 0;
        for (int k = 0; k < numBins; ++k)
        {
            const float m = level (k);
            const float left  = k > 0 ? level (k - 1) : 0.0f;
            const float next  = k + 1 < numBins ? level (k + 1) : 0.0f;
            if (m > 0.0f && m > left && m >= next)
                peaks[(size_t) numPeaks++] = k;
        }

        std::fill (outMagnitude.begin(), outMagnitude.end(), 0.0f);
        std::fill (strongest.begin(),    strongest.end(),    0.0f);
        if constexpr (stereo)
            std::fill (outMagnitudeR.begin(), outMagnitudeR.end(), 0.0f);

        for (int p = 0; p < numPeaks; ++p)
        {
            const int kp = peaks[(size_t) p];
            const int lo = p == 0 ? 0 : (peaks[(size_t) (p - 1)] + kp) / 2 + 1;
            const int hi = p + 1 == numPeaks ? numBins - 1 : (kp + peaks[(size_t) (p + 1)]) / 2;

            const double fIn  = (double) freq[kp];
            const double fOut = fIn * (double) ratio;
            const int shift   = ratio == 1.0f ? 0 : (int) std::lround (fOut - fIn);
            const int jp      = kp + shift;

            // First active frame: no phase history, so start from the input's
            // own phase at the peak, which makes the switch seamless.
            const bool continuing = s.wasActive && jp >= 0 && jp < numBins;
            const double thetaPeak = continuing
                ? (double) s.synthesisPhase[(size_t) jp] + expectedAdvance * fOut
                : (double) (phase != nullptr ? phase[kp] : analysisPhase[(size_t) kp]);

            for (int k = lo; k <= hi; ++k)
            {
                const int j = k + shift;
                const float m = level (k);
                if (j < 0 || j >= numBins || m <= 0.0f)
                    continue;

                const double offset = phase != nullptr ? (double) phase[k] - (double) phase[kp]
                                                       : pi * (double) (k - kp);

                // Regions landing on the same bins (a downward shift squeezes
                // them) add up; the phase comes from the strongest.
                outMagnitude[(size_t) j] += mag[k];
                if constexpr (stereo)
                    outMagnitudeR[(size_t) j] += src.magR[k];

                if (m > strongest[(size_t) j])
                {
                    strongest[(size_t) j] = m;
                    outPhase[(size_t) j]  = (float) wrap (thetaPeak + offset);
                    if constexpr (stereo)
                    {
                        outDeltaL[(size_t) j] = src.deltaL[k];
                        outDeltaR[(size_t) j] = src.deltaR[k];
                    }
                }
            }
        }

        for (int j = 0; j < numBins; ++j)
        {
            const auto i = (size_t) j;
            if (strongest[i] <= 0.0f)
            {
                frame[2 * j] = frame[2 * j + 1] = 0.0f;
                if constexpr (stereo)
                    right[2 * j] = right[2 * j + 1] = 0.0f;
                continue;
            }

            float theta, gain = 1.0f;
            if (wash)
            {
                const float u = detrand::u01 (washSeed, (uint64_t) band, frameCounter, (uint64_t) j);
                theta = (float) twoPi * u;
                gain = washGain;
            }
            else
            {
                theta = outPhase[i];
                s.synthesisPhase[i] = theta;
            }

            if constexpr (stereo)
            {
                // Both channels on the sum's phase, each with its own offset
                // to it, so the image survives the resynthesis.
                const float mL = outMagnitude[i] * gain, mR = outMagnitudeR[i] * gain;
                frame[2 * j]     = mL * std::cos (theta + outDeltaL[i]);
                frame[2 * j + 1] = mL * std::sin (theta + outDeltaL[i]);
                right[2 * j]     = mR * std::cos (theta + outDeltaR[i]);
                right[2 * j + 1] = mR * std::sin (theta + outDeltaR[i]);
            }
            else
            {
                const float m = outMagnitude[i] * gain;
                frame[2 * j]     = m * std::cos (theta);
                frame[2 * j + 1] = m * std::sin (theta);
            }
        }
    }

    static constexpr uint64_t washSeed = 0x5eedf2eeU;

    int size = 2048, hop = 512, numBins = 1025, numBands = 0;
    double sampleRate = 48000.0;
    double expectedAdvance = twoPi * 0.25;
    float washGain = 2.0f;

    bool analysedLastHop = false;
    bool frequenciesTracked = false;
    uint64_t frameCounter = 0;

    std::vector<float> analysisPhase, previousPhase, trueFrequency, magnitude;
    std::vector<float> outMagnitude, outPhase, strongest;
    std::vector<float> magnitudeL, magnitudeR, deltaL, deltaR;          // stereo analysis
    std::vector<float> outMagnitudeR, outDeltaL, outDeltaR;             // stereo synthesis
    std::vector<int> peaks;

    std::vector<Settings> settings;
    std::vector<BandState> state;
};

} // namespace fxme
