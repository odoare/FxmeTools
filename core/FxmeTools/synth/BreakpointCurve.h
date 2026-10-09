/*
  ------------------------------------------------------------------------------
    synth/BreakpointCurve.h

    A drawn modulation curve: breakpoints with a curvature per segment, an
    optional sustain region and an optional release region.

      CurveShape        the data (points in [0, 1] x [0, 1], time normalised
                        to the curve's total duration).
      SharedCurveShape  hands a CurveShape from the editing thread to the
                        audio thread without locks (a sequence lock over
                        atomics: the reader copies it when it has changed and
                        retries next block if it caught a write half done).
      CurvePlayer       one playhead (one per voice, or one global one).

    Playback:
      - Without a sustain region the curve plays once and holds its last
        value.
      - With one, while the note is held it loops between the sustain start
        and end points, or holds the value at the end point (sustainLoop
        false).
      - On note-off, with a release region enabled, the playhead leaves the
        sustain region (from its end if it was looping or holding there) and
        plays to the end; isReleasing() is true until it gets there, so a
        voice can wait for it. Without a release region note-off changes
        nothing: the curve keeps looping or holding.
      - setPosition() places the playhead directly, for a curve following the
        host's timeline (the owner computes the position from the PPQ).

    The segment shape is CurveAdsr::shape. Header-only, no allocation,
    realtime safe.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/synth/CurveAdsr.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace fxme
{

//==============================================================================
struct CurveShape
{
    static constexpr int maxPoints = 32;

    int numPoints = 2;
    std::array<float, maxPoints> x {}, y {}, curve {};   // curve[i]: segment i -> i + 1

    bool sustainEnabled = false;
    bool sustainLoop = true;
    int sustainStart = 0, sustainEnd = 1;
    bool releaseEnabled = false;

    CurveShape()
    {
        x[0] = 0.0f; y[0] = 0.0f;
        x[1] = 1.0f; y[1] = 1.0f;
    }

    /** Keeps the data usable whatever was written: at least two points, x
        sorted, starting at 0 and ending at 1, regions inside the points. */
    void sanitise() noexcept
    {
        numPoints = numPoints < 2 ? 2 : (numPoints > maxPoints ? maxPoints : numPoints);
        x[0] = 0.0f;
        x[(std::size_t) numPoints - 1] = 1.0f;
        for (int i = 1; i < numPoints; ++i)
            if (x[(std::size_t) i] < x[(std::size_t) i - 1])
                x[(std::size_t) i] = x[(std::size_t) i - 1];
        for (int i = 0; i < numPoints; ++i)
        {
            y[(std::size_t) i] = y[(std::size_t) i] < 0.0f ? 0.0f : (y[(std::size_t) i] > 1.0f ? 1.0f : y[(std::size_t) i]);
            curve[(std::size_t) i] = curve[(std::size_t) i] < -1.0f ? -1.0f : (curve[(std::size_t) i] > 1.0f ? 1.0f : curve[(std::size_t) i]);
        }
        sustainStart = sustainStart < 0 ? 0 : (sustainStart > numPoints - 1 ? numPoints - 1 : sustainStart);
        sustainEnd = sustainEnd < sustainStart ? sustainStart : (sustainEnd > numPoints - 1 ? numPoints - 1 : sustainEnd);
    }

    /** The curve at normalised time t. */
    float valueAt (float t) const noexcept
    {
        if (t <= x[0])
            return y[0];
        for (int i = 0; i < numPoints - 1; ++i)
        {
            const float x0 = x[(std::size_t) i], x1 = x[(std::size_t) i + 1];
            if (t < x1)
            {
                const float u = x1 > x0 ? (t - x0) / (x1 - x0) : 1.0f;
                const float y0 = y[(std::size_t) i], y1 = y[(std::size_t) i + 1];
                return y0 + (y1 - y0) * CurveAdsr::shape (u, curve[(std::size_t) i]);
            }
        }
        return y[(std::size_t) numPoints - 1];
    }
};

//==============================================================================
class SharedCurveShape
{
public:
    SharedCurveShape() { write (CurveShape()); }

    /** Publishes a new shape. One writer thread at a time. */
    void write (const CurveShape& s) noexcept
    {
        const auto v = seq.load (std::memory_order_relaxed);
        seq.store (v + 1, std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_release);

        numPoints.store (s.numPoints, std::memory_order_relaxed);
        for (int i = 0; i < CurveShape::maxPoints; ++i)
        {
            xs[(std::size_t) i].store (s.x[(std::size_t) i], std::memory_order_relaxed);
            ys[(std::size_t) i].store (s.y[(std::size_t) i], std::memory_order_relaxed);
            cs[(std::size_t) i].store (s.curve[(std::size_t) i], std::memory_order_relaxed);
        }
        flags.store ((s.sustainEnabled ? 1 : 0) | (s.sustainLoop ? 2 : 0) | (s.releaseEnabled ? 4 : 0),
                     std::memory_order_relaxed);
        regions.store ((s.sustainStart & 0xff) | ((s.sustainEnd & 0xff) << 8), std::memory_order_relaxed);

        seq.store (v + 2, std::memory_order_release);
    }

    /** Copies the shape into `dest` if it changed since `lastSeen`; returns
        true when it did. Realtime safe; a copy torn by a concurrent write is
        dropped and picked up on a later call. */
    bool readIfChanged (CurveShape& dest, std::uint32_t& lastSeen) const noexcept
    {
        const auto s1 = seq.load (std::memory_order_acquire);
        if ((s1 & 1u) != 0 || s1 == lastSeen)
            return false;

        CurveShape tmp;
        tmp.numPoints = numPoints.load (std::memory_order_relaxed);
        for (int i = 0; i < CurveShape::maxPoints; ++i)
        {
            tmp.x[(std::size_t) i] = xs[(std::size_t) i].load (std::memory_order_relaxed);
            tmp.y[(std::size_t) i] = ys[(std::size_t) i].load (std::memory_order_relaxed);
            tmp.curve[(std::size_t) i] = cs[(std::size_t) i].load (std::memory_order_relaxed);
        }
        const int f = flags.load (std::memory_order_relaxed);
        const int r = regions.load (std::memory_order_relaxed);

        std::atomic_thread_fence (std::memory_order_acquire);
        if (seq.load (std::memory_order_relaxed) != s1)
            return false;

        tmp.sustainEnabled = (f & 1) != 0;
        tmp.sustainLoop = (f & 2) != 0;
        tmp.releaseEnabled = (f & 4) != 0;
        tmp.sustainStart = r & 0xff;
        tmp.sustainEnd = (r >> 8) & 0xff;
        tmp.sanitise();
        dest = tmp;
        lastSeen = s1;
        return true;
    }

private:
    std::atomic<std::uint32_t> seq { 0 };
    std::atomic<int> numPoints { 2 }, flags { 0 }, regions { 0 };
    std::array<std::atomic<float>, CurveShape::maxPoints> xs {}, ys {}, cs {};
};

//==============================================================================
class CurvePlayer
{
public:
    void noteOn() noexcept
    {
        t = 0.0;
        released = false;
        finished = false;
    }

    void noteOff (const CurveShape& shape) noexcept
    {
        if (released)
            return;
        released = true;

        if (! (shape.sustainEnabled && shape.releaseEnabled))
        {
            released = false;          // note-off does nothing to this curve
            return;
        }

        // From the sustain region (looping or holding), leave by its end.
        const double sEnd = (double) shape.x[(std::size_t) shape.sustainEnd];
        if (t >= (double) shape.x[(std::size_t) shape.sustainStart] && t <= sEnd)
            t = sEnd;
    }

    /** Advances by `dt` in normalised time (seconds / duration * speed). */
    float advance (const CurveShape& shape, double dt) noexcept
    {
        if (! finished)
        {
            t += dt;

            const bool sustaining = shape.sustainEnabled && ! released;
            if (sustaining)
            {
                const double s0 = (double) shape.x[(std::size_t) shape.sustainStart];
                const double s1 = (double) shape.x[(std::size_t) shape.sustainEnd];
                if (t >= s1)
                {
                    if (shape.sustainLoop && s1 - s0 > 1.0e-6)
                        t = s0 + std::fmod (t - s0, s1 - s0);
                    else
                        t = s1;
                }
            }
            else if (t >= 1.0)
            {
                t = 1.0;
                finished = true;
            }
        }
        return shape.valueAt ((float) t);
    }

    /** Places the playhead at normalised position `position` (host sync). */
    float setPosition (const CurveShape& shape, double position) noexcept
    {
        t = position - std::floor (position);
        return shape.valueAt ((float) t);
    }

    /** True while a release region is playing out after note-off. */
    bool isReleasing() const noexcept { return released && ! finished; }
    bool isFinished() const noexcept  { return finished; }
    double getPosition() const noexcept { return t; }

private:
    double t = 0.0;
    bool released = false, finished = false;
};

} // namespace fxme
