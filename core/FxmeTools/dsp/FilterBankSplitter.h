/*
  ------------------------------------------------------------------------------
    FilterBankSplitter.h

    The zero-latency twin of fxme::SpectralBandSplitter: splits one mono
    stream into bands with filters instead of a short-time Fourier transform.
    Per band, a fxme::EdgeBandPass (24 dB per octave on each side) cuts the
    band out of the input, a fxme::BandDynamicsProcessor applies its gate and
    ceiling (expander, compressor or cut) to the band's level, and the band's
    gain (and pan, when applied) finish it.

    It takes the same fxme::SpectralBand settings and has the same outward
    API as the spectral splitter, so a consumer can offer both and switch
    between them with little code. What the filters give up, compared with
    the spectrum:

      - the gate and the ceiling act on the whole band's level, not bin by
        bin (a multiband gate: a band cannot keep only its peaks);
      - the edges are 24 dB per octave rather than near brick-wall, so
        neighbouring bands overlap and a band narrower than about an octave
        loses some level;
      - there is no frame to process, so no spectral effects.

    What it gains: no latency and transients intact.

    Mono or stereo input (prepare()'s numInputChannels). In stereo each band
    is filtered on both channels, and its gate follows the two channels'
    average (the mono sum a spectrum view would show), with one gain for
    both, so the band opens and closes as one and keeps its image.

    Levels follow fxme::BandDynamicsProcessor's convention, so a steady tone passes a gate
    drawn at the same place in either splitter. A noisy band's level (the sum
    of all its bins) reads higher than its spectrum trace: getBandLevelDb()
    is what the gate compares, for a consumer to show beside the lines.

    Threading: prepare() allocates (message thread / prepareToPlay).
    setBand(), setGateTimes(), setGateKnee(), setApplyPan() and process() are
    realtime safe, from the audio thread. getGateOpenness() and
    getBandLevelDb() are safe from any thread.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/dsp/BandDynamicsProcessor.h>
#include <FxmeTools/dsp/EdgeBandPass.h>
#include <FxmeTools/dsp/SpectralBandSplitter.h>
#include <FxmeTools/util/AudioBuffer.h>
#include <FxmeTools/util/Math.h>
#include <FxmeTools/util/SmoothedValue.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <memory>
#include <vector>

namespace fxme
{

class FilterBankSplitter
{
public:
    FilterBankSplitter() = default;

    //==========================================================================
    /** Allocates everything. Message thread / prepareToPlay only.
        `numInputChannels` is 1 (process (mono, n)) or 2 (process (left,
        right, n)). */
    void prepare (double sampleRateIn, int maxBlockSize, int numBandsIn, int numInputChannels = 1)
    {
        sampleRate = sampleRateIn > 0.0 ? sampleRateIn : 48000.0;
        numBands   = fxme::jmax (0, numBandsIn);
        blockSize  = fxme::jmax (1, maxBlockSize);
        stereoInput = numInputChannels > 1;
        detector.assign ((size_t) blockSize, 0.0f);

        bands.assign ((size_t) numBands, {});
        state.clear();
        state.resize ((size_t) numBands);
        for (auto& s : state)
        {
            s.filter.prepare (sampleRate);
            s.filterR.prepare (sampleRate);
            s.dynamics.prepare (sampleRate);
        }

        outputs.setSize (fxme::jmax (1, 2 * numBands), blockSize);
        outputs.clear();

        openness = std::make_unique<std::atomic<float>[]> ((size_t) fxme::jmax (1, numBands));
        levels   = std::make_unique<std::atomic<float>[]> ((size_t) fxme::jmax (1, numBands));
        peaks    = std::make_unique<std::atomic<float>[]> ((size_t) fxme::jmax (1, numBands));

        setGateTimes (gateAttackSeconds, gateReleaseSeconds);
        setLevelSmoothingSeconds (0.02);
        reset();
    }

    /** Clears every filter and gate; keeps the band settings. */
    void reset()
    {
        for (int b = 0; b < numBands; ++b)
        {
            auto& s = state[(size_t) b];
            s.filter.reset();
            s.filterR.reset();
            s.dynamics.reset();
            s.wasEnabled = false;
            s.gainL.setCurrentAndTargetValue (s.gainL.getTargetValue());
            s.gainR.setCurrentAndTargetValue (s.gainR.getTargetValue());
            openness[(size_t) b].store (0.0f);
            levels[(size_t) b].store (-200.0f);
            peaks[(size_t) b].store (-200.0f);
        }
        outputs.clear();
    }

    int    getNumBands() const noexcept          { return numBands; }
    double getSampleRate() const noexcept        { return sampleRate; }

    /** None: the filters work sample by sample. */
    int getLatencySamples() const noexcept       { return 0; }

    /** The band's gate gain, 0 to 1: 1 with no level gating, 0 for a
        disabled band. Any thread. */
    float getGateOpenness (int band) const noexcept
    {
        return openness != nullptr && fxme::isPositiveAndBelow (band, numBands)
                 ? openness[(size_t) band].load (std::memory_order_relaxed)
                 : 0.0f;
    }

    /** The band's level in dB, as its gate compares it (see fxme::BandDynamicsProcessor),
        at the end of the last process() call. Any thread. */
    float getBandLevelDb (int band) const noexcept
    {
        return levels != nullptr && fxme::isPositiveAndBelow (band, numBands)
                 ? levels[(size_t) band].load (std::memory_order_relaxed)
                 : -200.0f;
    }

    /** The band's highest level in dB since the previous call (sample
        accurate, same convention as getBandLevelDb()), then starts over: for
        a peak-hold marker read on a GUI timer. Meant for one reader. A peak
        landing between the read and the reset can be lost; harmless for a
        display. */
    float takeBandPeakLevelDb (int band) noexcept
    {
        return peaks != nullptr && fxme::isPositiveAndBelow (band, numBands)
                 ? peaks[(size_t) band].exchange (-200.0f, std::memory_order_relaxed)
                 : -200.0f;
    }

    //==========================================================================
    void setBand (int index, const SpectralBand& b) noexcept
    {
        if (! fxme::isPositiveAndBelow (index, numBands))
            return;

        bands[(size_t) index] = b;
        auto& s = state[(size_t) index];

        const float lo = fxme::jmin (b.lowHz, b.highHz);
        const float hi = fxme::jmax (b.lowHz, b.highHz);
        s.filter.setEdges (lo, hi, ! s.wasEnabled);
        s.filterR.setEdges (lo, hi, ! s.wasEnabled);
        s.dynamics.setThresholds (b.gateDb, b.ceilingDb);
        s.dynamics.setDetectorSeconds (BandDynamicsProcessor::detectorSecondsFor (lo));
        updateGainTargets (index);
    }

    SpectralBand getBand (int index) const noexcept
    {
        return fxme::isPositiveAndBelow (index, numBands) ? bands[(size_t) index]
                                                          : SpectralBand {};
    }

    /** As fxme::SpectralBandSplitter::setApplyPan(). */
    void setApplyPan (bool shouldApplyPan) noexcept
    {
        if (applyPan == shouldApplyPan)
            return;

        applyPan = shouldApplyPan;
        for (int b = 0; b < numBands; ++b)
            updateGainTargets (b);
    }

    bool isApplyingPan() const noexcept          { return applyPan; }

    /** One band's dynamics around its two lines (see fxme::BandDynamics):
        with a compressor ratio on the ceiling, a classic multiband
        compressor. Per sample. */
    void setBandDynamics (int index, const BandDynamics& d) noexcept
    {
        if (fxme::isPositiveAndBelow (index, numBands))
            state[(size_t) index].dynamics.setDynamics (d);
    }

    /** Attack and release of every band's gate (defaults 5 ms and 80 ms).
        Applied per sample, so any attack is meaningful. */
    void setGateTimes (float attackSeconds, float releaseSeconds) noexcept
    {
        gateAttackSeconds  = fxme::jmax (0.0f, attackSeconds);
        gateReleaseSeconds = fxme::jmax (0.0f, releaseSeconds);
        for (auto& s : state)
            s.dynamics.setTimes (gateAttackSeconds, gateReleaseSeconds);
    }

    /** Knee of every band's gate and ceiling, in dB. */
    void setGateKnee (float kneeDb) noexcept
    {
        for (auto& s : state)
            s.dynamics.setKnee (kneeDb);
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
    /** Feeds `numSamples` mono samples in and renders every band's stereo
        output for them. A disabled band outputs silence and costs nothing
        else; enabled again, it starts from rest. */
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

    /** The last process() call's output for one band, channel 0 or 1. */
    const float* getBandOutput (int band, int channel) const noexcept
    {
        const int ch = fxme::jlimit (0, fxme::jmax (0, outputs.getNumChannels() - 1),
                                     2 * band + channel);
        return outputs.getReadPointer (ch);
    }

private:
    void processInput (const float* inL, const float* inR, int numSamples) noexcept
    {
        assert (numSamples <= outputs.getNumSamples());
        numSamples = fxme::jmin (numSamples, outputs.getNumSamples());
        if (numSamples <= 0 || numBands == 0)
            return;

        float* const* out = outputs.getArrayOfWritePointers();

        for (int b = 0; b < numBands; ++b)
        {
            auto& s = state[(size_t) b];
            float* left  = out[2 * b];
            float* right = out[2 * b + 1];

            if (! bands[(size_t) b].enabled)
            {
                std::fill (left,  left  + numSamples, 0.0f);
                std::fill (right, right + numSamples, 0.0f);
                if (s.wasEnabled)
                {
                    s.wasEnabled = false;
                    s.filter.reset();
                    s.filterR.reset();
                    s.dynamics.reset();
                }
                for (int i = 0; i < numSamples && (s.gainL.isSmoothing() || s.gainR.isSmoothing()); ++i)
                {
                    s.gainL.getNextValue();
                    s.gainR.getNextValue();
                }
                openness[(size_t) b].store (0.0f, std::memory_order_relaxed);
                levels[(size_t) b].store (-200.0f, std::memory_order_relaxed);
                continue;
            }

            if (! s.wasEnabled)
            {
                // Back from disabled: the edges jump to where the band is now
                // rather than gliding from where it was.
                const auto& cfg = bands[(size_t) b];
                s.filter.setEdges (cfg.lowHz, cfg.highHz, true);
                s.filterR.setEdges (cfg.lowHz, cfg.highHz, true);
                s.wasEnabled = true;
            }

            std::copy (inL, inL + numSamples, left);
            s.filter.process (left, numSamples);

            if (inR != nullptr)
                renderStereoBand (s, left, right, inR, numSamples);
            else
                renderMonoBand (s, left, right, numSamples);

            openness[(size_t) b].store (s.dynamics.getGain(), std::memory_order_relaxed);
            levels[(size_t) b].store (s.dynamics.getLevelDb(), std::memory_order_relaxed);

            const float peak = s.dynamics.takePeakLevelDb();
            if (peak > peaks[(size_t) b].load (std::memory_order_relaxed))
                peaks[(size_t) b].store (peak, std::memory_order_relaxed);
        }
    }

    /** Mono input: `left` holds the filtered band; gate it, then spread it
        over both outputs with the band's gain and pan. */
    template <typename State>
    static void renderMonoBand (State& s, float* left, float* right, int numSamples) noexcept
    {
        s.dynamics.process (left, numSamples);

        if (s.gainL.isSmoothing() || s.gainR.isSmoothing())
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const float v = left[i];
                left[i]  = v * s.gainL.getNextValue();
                right[i] = v * s.gainR.getNextValue();
            }
        }
        else
        {
            const float gl = s.gainL.getCurrentValue();
            const float gr = s.gainR.getCurrentValue();
            for (int i = 0; i < numSamples; ++i)
            {
                const float v = left[i];
                left[i]  = v * gl;
                right[i] = v * gr;
            }
        }
    }

    /** Stereo input: `left` holds the filtered left band; filter the right
        one, gate both on their average, then each side its gain. */
    template <typename State>
    void renderStereoBand (State& s, float* left, float* right, const float* inR, int numSamples) noexcept
    {
        std::copy (inR, inR + numSamples, right);
        s.filterR.process (right, numSamples);

        for (int i = 0; i < numSamples; ++i)
            detector[(size_t) i] = 0.5f * (left[i] + right[i]);
        s.dynamics.processLinked (detector.data(), left, right, numSamples);

        for (int i = 0; i < numSamples; ++i)
        {
            left[i]  *= s.gainL.getNextValue();
            right[i] *= s.gainR.getNextValue();
        }
    }

    struct BandState
    {
        EdgeBandPass filter, filterR;    // filterR only with stereo input
        BandDynamicsProcessor dynamics;
        bool wasEnabled = false;
        SmoothedValue<float> gainL { 0.0f }, gainR { 0.0f };
    };

    /** As in fxme::SpectralBandSplitter: gain and constant-power pan folded
        into two smoothed targets, or the gain alone on both sides. */
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

        const float theta = (fxme::jlimit (-1.0f, 1.0f, b.pan) + 1.0f)
                                * fxme::MathConstants<float>::pi * 0.25f;
        s.gainL.setTargetValue (g * std::cos (theta));
        s.gainR.setTargetValue (g * std::sin (theta));
    }

    std::vector<SpectralBand> bands;
    std::vector<BandState> state;
    AudioBuffer outputs;

    double sampleRate = 48000.0;
    int numBands = 0, blockSize = 512;
    float gateAttackSeconds = 0.005f, gateReleaseSeconds = 0.080f;
    bool applyPan = true;
    bool stereoInput = false;
    std::vector<float> detector;         // the stereo gate's input, one block

    std::unique_ptr<std::atomic<float>[]> openness, levels, peaks;

    FilterBankSplitter (const FilterBankSplitter&) = delete;
    FilterBankSplitter& operator= (const FilterBankSplitter&) = delete;
};

} // namespace fxme
