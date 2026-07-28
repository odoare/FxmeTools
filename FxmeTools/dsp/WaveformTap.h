/*
  ------------------------------------------------------------------------------
    WaveformTap.h

    Lock-free single-writer/single-reader signal tap feeding a time-domain
    display (WaveformDisplay). The audio thread pushes samples into a ring
    buffer; the GUI thread snapshots the most recent N samples. A monotonic
    sample counter lets the reader place the snapshot on a time axis.

    Unlike SpectrumTap the ring length is chosen by the owner (seconds of
    history to keep): call prepare() from prepareToPlay — it allocates and is
    NOT audio-thread-safe — then push() from the audio thread.

    One tap carries one channel; use one tap (and one display trace) per
    monitored signal.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <vector>

namespace fxme
{

class WaveformTap
{
public:
    WaveformTap() = default;

    /** Allocates a ring holding `seconds` of history at `newSampleRate`.
        Call from prepareToPlay (message/prepare thread) — NOT while the audio
        thread may be inside push(). */
    void prepare (double newSampleRate, double seconds)
    {
        sampleRate = newSampleRate;
        const int n = juce::jmax (1024, (int) std::ceil (seconds * newSampleRate));
        buffer.assign ((size_t) n, 0.0f);
        size = n;
        writePos.store (0);
        totalPushed.store (0);
    }

    void setEnabled (bool e) noexcept       { enabled.store (e); }
    bool isEnabled() const noexcept         { return enabled.load(); }

    double getSampleRate() const noexcept   { return sampleRate; }
    int getCapacity() const noexcept        { return size; }

    /** Total samples pushed since prepare() — a monotonic clock for the
        display's time axis. */
    juce::int64 getTotalPushed() const noexcept
    {
        return totalPushed.load (std::memory_order_acquire);
    }

    /** Audio thread. */
    void push (const float* data, int n) noexcept
    {
        if (! enabled.load() || size == 0)
            return;

        int w = writePos.load (std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
        {
            buffer[(size_t) w] = data[i];
            if (++w >= size)
                w = 0;
        }
        writePos.store (w, std::memory_order_release);
        totalPushed.store (totalPushed.load (std::memory_order_relaxed) + n,
                           std::memory_order_release);
    }

    /** Copies the most recent `count` samples in chronological order,
        zero-padding the (leading) part not yet written. */
    void snapshot (float* dest, int count) const noexcept
    {
        if (size == 0 || count <= 0)
            return;

        count = juce::jmin (count, size);
        int w = writePos.load (std::memory_order_acquire);
        int r = w - count;
        if (r < 0)
            r += size;
        for (int i = 0; i < count; ++i)
        {
            dest[i] = buffer[(size_t) r];
            if (++r >= size)
                r = 0;
        }
    }

private:
    std::vector<float> buffer;
    int size = 0;
    double sampleRate = 44100.0;
    std::atomic<int> writePos { 0 };
    std::atomic<juce::int64> totalPushed { 0 };
    std::atomic<bool> enabled { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformTap)
};

} // namespace fxme
