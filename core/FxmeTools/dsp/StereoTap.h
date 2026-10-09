/*
  ------------------------------------------------------------------------------
    dsp/StereoTap.h

    Lock-free tap of a stereo signal for a display: the audio thread pushes
    frames into a ring, the GUI copies the most recent ones, left and right
    kept aligned (which two WaveformTaps could not guarantee). Made for a
    vectorscope (Lissajous) and for oscilloscopes; a mono signal pushes the
    same pointer twice.

    Single producer (audio), any number of readers; a reader racing the
    writer may see a few frames from the next lap at the oldest end of its
    copy, which a display never notices. prepare() allocates and must not
    race push().

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace fxme
{

class StereoTap
{
public:
    void prepare (int capacityFrames)
    {
        capacity = capacityFrames < 64 ? 64 : capacityFrames;
        left.assign ((std::size_t) capacity, 0.0f);
        right.assign ((std::size_t) capacity, 0.0f);
        writePos.store (0);
        total.store (0);
    }

    int getCapacity() const noexcept { return capacity; }

    /** Audio thread. */
    void push (const float* l, const float* r, int n) noexcept
    {
        if (capacity == 0)
            return;
        int w = writePos.load (std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
        {
            left[(std::size_t) w] = l[i];
            right[(std::size_t) w] = r != nullptr ? r[i] : l[i];
            if (++w >= capacity)
                w = 0;
        }
        writePos.store (w, std::memory_order_release);
        total.fetch_add (n, std::memory_order_release);
    }

    /** Pushes one frame (audio thread). */
    void push (float l, float r) noexcept { push (&l, &r, 1); }

    /** Copies the most recent `count` frames, oldest first. */
    void snapshot (float* destL, float* destR, int count) const noexcept
    {
        if (capacity == 0 || count <= 0)
            return;
        count = count > capacity ? capacity : count;
        int r = writePos.load (std::memory_order_acquire) - count;
        if (r < 0)
            r += capacity;
        for (int i = 0; i < count; ++i)
        {
            destL[i] = left[(std::size_t) r];
            destR[i] = right[(std::size_t) r];
            if (++r >= capacity)
                r = 0;
        }
    }

    /** Frames pushed since prepare(): lets a display tell whether anything
        new arrived. */
    std::int64_t getTotalPushed() const noexcept { return total.load (std::memory_order_acquire); }

private:
    std::vector<float> left, right;
    int capacity = 0;
    std::atomic<int> writePos { 0 };
    std::atomic<std::int64_t> total { 0 };
};

} // namespace fxme
