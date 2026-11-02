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

#include <FxmeTools/util/Math.h>
#include <array>
#include <atomic>
#include <cstdint>

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
        totalPushed.store (totalPushed.load (std::memory_order_relaxed) + n,
                           std::memory_order_release);
    }

    /** Total samples pushed since construction: sample i sits at ring index
        i % size, so a reader can stream the tap without missing or repeating
        samples (see readSince). */
    std::int64_t getTotalPushed() const noexcept
    {
        return totalPushed.load (std::memory_order_acquire);
    }

    /** Streams the tap (FxmeNet source side, reader thread). Copies up to
        `maxCount` samples pushed from sample index `from` on, oldest first,
        and advances `from` past them. A `from` older than half the ring (the
        part the writer cannot be overwriting) jumps forward: the samples in
        between are skipped, not returned. Returns the number copied. */
    int readSince (std::int64_t& from, float* dest, int maxCount) const noexcept
    {
        const auto total = totalPushed.load (std::memory_order_acquire);
        from = fxme::jlimit (total - size / 2, total, from);
        const int n = (int) fxme::jmin ((std::int64_t) maxCount, total - from);
        int r = (int) (from % size);
        for (int i = 0; i < n; ++i)
        {
            dest[i] = buffer[(size_t) r];
            if (++r >= size)
                r = 0;
        }
        from += n;
        return n;
    }

    /** Copies the most recent `count` samples in chronological order. */
    void snapshot (float* dest, int count) const noexcept
    {
        count = fxme::jlimit (1, size, count);
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
    std::atomic<std::int64_t> totalPushed { 0 };
    std::atomic<bool> enabled { false };
};

} // namespace fxme
