/*
  ------------------------------------------------------------------------------
    FirFilter.h

    Mono FIR convolver backed by the WDL convolution engine (zero latency),
    e.g. for per-output loudspeaker correction. The impulse response is loaded
    from a wav file on the message thread (with resampling to the session rate)
    and swapped under a lock.

    Threading
    ---------
    The loaders (prepare / loadFile / loadFromReader / setImpulse /
    clearImpulse) run on the message thread and hold `lock` across the whole
    swap, which includes reading the file, resampling and rebuilding the WDL
    engine. That is far too long for the audio thread to ever wait on, so
    process() takes the lock with a **try**-lock and simply leaves the block
    untouched (dry, exactly as when no impulse is loaded) if a load is in
    flight. Loading an IR therefore costs a few un-convolved blocks rather than
    a blocked audio callback and an xrun. hasImpulse() is lock-free (an atomic)
    so a caller can poll it per block without touching `lock` at all.

    getImpulseLength() still takes the lock and is message-thread only.

    NOTE: depends on WDL (FxmeTools' nested submodule). This header is therefore
    NOT part of the FxmeTools module umbrella — include it explicitly:
        #include <FxmeTools/dsp/FirFilter.h>
    and make sure the WDL sources (convoengine.cpp, fft.c, resample.cpp) are
    compiled into your target (the fxmetools_attach() CMake helper does this).

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>
#include "../../WDL/WDL/convoengine.h"

namespace fxme
{

class FirFilter
{
public:
    FirFilter() = default;

    void prepare (double sampleRate, int maxBlockSize)
    {
        juce::ScopedLock sl (lock);
        currentSampleRate = sampleRate;
        wdlInput.resize ((size_t) maxBlockSize);
        engine.Reset();
        if (sourceIR.getNumSamples() > 0)
            rebuildEngineImpulse();
        else
            loadSilentImpulse();
    }

    /** Load an IR wav file. Returns true on success. Message thread. */
    bool loadFile (const juce::File& file)
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
        return reader != nullptr && loadFromReader (*reader);
    }

    /** Load an IR from any AudioFormatReader — e.g. one created by
        EmbeddedAudio::createReader(), so an impulse embedded in the plugin
        state (presets, sessions) can feed the filter without a file on disk.
        Returns true on success. Message thread. */
    bool loadFromReader (juce::AudioFormatReader& reader)
    {
        if (reader.lengthInSamples <= 0 || reader.numChannels < 1)
            return false;

        const int n = (int) juce::jmin (reader.lengthInSamples, (juce::int64) (1 << 20));
        juce::AudioBuffer<float> temp ((int) reader.numChannels, n);
        reader.read (&temp, 0, n, 0, true, true);

        // Collapse to mono
        juce::AudioBuffer<float> mono (1, n);
        mono.clear();
        for (int c = 0; c < temp.getNumChannels(); ++c)
            mono.addFrom (0, 0, temp, c, 0, n, 1.0f / (float) temp.getNumChannels());

        {
            juce::ScopedLock sl (lock);
            sourceIR = std::move (mono);
            sourceRate = reader.sampleRate;
            rebuildEngineImpulse();
        }
        return true;
    }

    /** Use an in-memory IR (e.g. just exported from the analysis part). */
    void setImpulse (const juce::AudioBuffer<float>& ir, double rate)
    {
        juce::ScopedLock sl (lock);
        sourceIR.makeCopyOf (ir);
        sourceRate = rate;
        rebuildEngineImpulse();
    }

    void clearImpulse()
    {
        juce::ScopedLock sl (lock);
        sourceIR.setSize (0, 0);
        loadSilentImpulse();
    }

    /** Whether an impulse is loaded. Lock-free (atomic), so the audio thread can
        call this every block without contending with a load in progress. */
    bool hasImpulse() const noexcept    { return impulseLoaded.load(); }

    int getImpulseLength() const
    {
        juce::ScopedLock sl (lock);
        return sourceIR.getNumSamples();
    }

    /** Bulk delay the impulse imposes, in samples at the session rate: the
        position of its peak. Linear-phase correction IRs (centred at N/2) carry
        N/2; minimum-phase IRs carry ~0. Used for inter-output latency
        compensation. Lock-free (atomic). */
    int getLatencySamples() const noexcept  { return impulseLatency.load(); }

    /** In-place convolution of one channel. Audio thread: never blocks.
        Leaves `data` untouched (dry) when no impulse is loaded, or when a load
        is in flight and the try-lock fails. */
    void process (float* data, int n)
    {
        if (! impulseLoaded.load())
            return;     // bypass when no IR loaded — lock-free fast path

        // Try, never wait: the message thread holds this lock across a file
        // read + resample + engine rebuild. Blocking here would be an xrun.
        const juce::ScopedTryLock tl (lock);
        if (! tl.isLocked())
            return;     // a load is swapping the impulse; stay dry this block

        if (sourceIR.getNumSamples() == 0)
            return;     // cleared between the atomic check and the lock

        if ((int) wdlInput.size() < n)
            return;     // should not happen (prepare sizes it)

        for (int i = 0; i < n; ++i)
            wdlInput[(size_t) i] = (WDL_FFT_REAL) data[i];

        WDL_FFT_REAL* in = wdlInput.data();
        engine.Add (&in, n, 1);

        const int avail = engine.Avail (n);
        WDL_FFT_REAL** out = engine.Get();
        const int toCopy = juce::jmin (avail, n);

        for (int i = 0; i < toCopy; ++i)
            data[i] = (float) out[0][i];
        for (int i = toCopy; i < n; ++i)
            data[i] = 0.0f;

        engine.Advance (toCopy);
    }

private:
    void rebuildEngineImpulse()
    {
        const int n = sourceIR.getNumSamples();
        if (n <= 0)
        {
            loadSilentImpulse();
            return;
        }

        // Resample to the session rate if needed.
        juce::AudioBuffer<float> resampled;
        const float* src = sourceIR.getReadPointer (0);
        int len = n;

        if (sourceRate > 0 && currentSampleRate > 0
            && std::abs (sourceRate - currentSampleRate) > 1.0)
        {
            const double ratio = sourceRate / currentSampleRate;
            const int outLen = (int) ((double) n / ratio) + 1;
            resampled.setSize (1, outLen);
            juce::LagrangeInterpolator interp;
            interp.process (ratio, src, resampled.getWritePointer (0), outLen);
            src = resampled.getReadPointer (0);
            len = outLen;
        }

        impulseBuffer.SetNumChannels (1);
        impulseBuffer.SetLength (len);
        impulseBuffer.samplerate = currentSampleRate;
        auto* dst = impulseBuffer.impulses[0].Get();
        int peakIdx = 0;
        float peakVal = 0.0f;
        for (int i = 0; i < len; ++i)
        {
            dst[i] = (WDL_FFT_REAL) src[i];
            const float a = std::abs (src[i]);
            if (a > peakVal) { peakVal = a; peakIdx = i; }
        }
        impulseLatency.store (peakIdx);

        engine.SetImpulse (&impulseBuffer);

        // Published last: process() reads this without the lock, so it must not
        // see "loaded" before the engine actually holds the impulse.
        impulseLoaded.store (true);
    }

    void loadSilentImpulse()
    {
        // Cleared first: process() must stop taking the convolution path before
        // the engine's impulse goes away, not after.
        impulseLoaded.store (false);
        impulseBuffer.SetNumChannels (1);
        impulseBuffer.SetLength (1);
        impulseBuffer.samplerate = currentSampleRate;
        impulseBuffer.impulses[0].Get()[0] = 0.0;
        impulseLatency.store (0);
        engine.SetImpulse (&impulseBuffer);
    }

    mutable juce::CriticalSection lock;
    WDL_ImpulseBuffer impulseBuffer;
    WDL_ConvolutionEngine_Div engine;
    std::vector<WDL_FFT_REAL> wdlInput;

    juce::AudioBuffer<float> sourceIR;   // as loaded (mono)
    double sourceRate = 0.0;
    double currentSampleRate = 44100.0;
    std::atomic<int> impulseLatency { 0 };   // peak position, session samples
    // Mirrors "sourceIR.getNumSamples() > 0" so hasImpulse() and process()'s
    // fast path need no lock. Written only under `lock`, by the two functions
    // that install an impulse into the engine.
    std::atomic<bool> impulseLoaded { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FirFilter)
};

} // namespace fxme
