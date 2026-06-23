/*
  ------------------------------------------------------------------------------
    SpectrumTap.h

    Lock-free single-writer/single-reader signal tap feeding a spectrum
    analyzer. The audio thread pushes samples into a ring buffer; the GUI thread
    snapshots the most recent fftSize samples and performs the FFT itself
    (windowing + averaging happen on the GUI side, in SpectrumAnalyzer).

    Pure, app-agnostic: a single tap plus the shared FFT-size constants. The
    routing of a set of taps (which signal feeds which tap) is application
    specific and lives in the consumer.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>

namespace fxme
{

constexpr int spectrumFftOrder    = 13;
constexpr int spectrumFftSize     = 1 << spectrumFftOrder;  // 4096 (default window)
constexpr int spectrumMinFftOrder = 10;                     // 1024
constexpr int spectrumMaxFftOrder = 15;                     // 32768
constexpr int spectrumMaxFftSize  = 1 << spectrumMaxFftOrder;

class SpectrumTap
{
public:
    SpectrumTap()
    {
        buffer.fill (0.0f);
    }

    void setEnabled (bool e) noexcept       { enabled.store (e); }
    bool isEnabled() const noexcept         { return enabled.load(); }

    void push (const float* data, int n) noexcept
    {
        if (! enabled.load())
            return;

        int w = writePos.load (std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
        {
            buffer[(size_t) w] = data[i];
            if (++w >= size)
                w = 0;
        }
        writePos.store (w, std::memory_order_release);
    }

    /** Copies the most recent `count` samples in chronological order. */
    void snapshot (float* dest, int count) const noexcept
    {
        count = juce::jlimit (1, size, count);
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
    static constexpr int size = 2 * spectrumMaxFftSize;
    std::array<float, (size_t) size> buffer;
    std::atomic<int> writePos { 0 };
    std::atomic<bool> enabled { false };
};

} // namespace fxme
