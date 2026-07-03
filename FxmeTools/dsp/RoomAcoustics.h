/*
  ------------------------------------------------------------------------------
    RoomAcoustics.h

    Room-acoustics building blocks shared across FX-Mechanics projects:
    the image-source model for a parallelepipedic (box) room, Sabine's
    reverberation-time formula, and the small helpers that connect them to a
    delay-tap / convolution renderer. Header-only and JUCE-free — depends only
    on the standard library — so it can be reused from any C++ project.

    Conventions
    -----------
      * Right-handed frame, x / y / z aligned with the box edges, all positions
        in metres. The box spans [0, rx] x [0, ry] x [0, rz].
      * The six walls are indexed by the Wall enum (x/y/z, min/max side).
      * Wall damping is the fraction of *amplitude* absorbed per reflection
        (0 = fully reflective, 1 = fully absorbent). hfDamp is an extra
        high-frequency loss per reflection, accumulated into a one-pole
        low-pass coefficient per image source.

    API summary
    -----------
      Wall, numWalls               Wall indexing for a box room.
      BoxRoom                      Geometry + per-wall damping description.
      volume (room)                Box volume (m^3).
      sabineRT60 (room)            Reverberation time from Sabine's formula:
                                   RT60 = 0.161 V / A, A = sum of area x damp.
      t60DecayPerSample (rt60, sr) Exponential decay rate k so that
                                   exp(-k n) drops 60 dB over rt60 seconds.
      hfDampingAlpha (hfSum, sr)   One-pole low-pass coefficient for an
                                   accumulated HF damping (alpha = 1 means
                                   transparent; y[n] = a x[n] + (1-a) y[n-1]).
      ImageTap                     One image source: integer sample delay,
                                   broadband gain (reflection losses and 1/r),
                                   one-pole HF coefficient, unit direction of
                                   arrival in the room frame, direct flag.
      ImageSourceConfig            Tap count / sample rate / thresholds.
      computeImageSources (...)    Enumerate the image sources of one
                                   listener/source pair, sorted by arrival
                                   time and truncated to the requested count.
                                   Directions are in the ROOM frame — apply a
                                   listener rotation downstream (see
                                   Ambisonics.h ShRotation) so head moves do
                                   not require recomputation.
      TailCrossfade                Fade-out window over the last taps, used to
      tailCrossfade (taps, frac)   hand over smoothly to a diffuse late field;
      crossfadeGain (window, i)    returns the per-tap fade factor (1 -> 0).

    Typical use: hybrid early-reflection + diffuse-tail reverb
    ----------------------------------------------------------
        fxme::room::BoxRoom room { rx, ry, rz, damp, hfDamp };
        std::vector<fxme::room::ImageTap> taps;
        fxme::room::computeImageSources (room, listenerPos, sourcePos,
                                         config, taps);
        auto window = fxme::room::tailCrossfade (taps);   // last 10 % fade out
        // ... render taps with a multitap delay; start the diffuse tail at
        // window.startSamples, fading it in until window.endSamples, with an
        // envelope exp(-t60DecayPerSample(sabineRT60(room), sr) * n).

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include "Ambisonics.h"   // Vec3 / normalise (direction of arrival)

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace fxme::room
{

// The six walls of the box, indexed for per-wall damping.
enum Wall { wallXmin = 0, wallXmax, wallYmin, wallYmax, wallZmin, wallZmax, numWalls };

//==============================================================================
// Box-room description: dimensions (metres) and per-wall damping.
struct BoxRoom
{
    float rx = 5.0f, ry = 4.0f, rz = 3.0f;
    std::array<float, numWalls> damp   {};   // broadband, 0..1 per reflection
    std::array<float, numWalls> hfDamp {};   // extra HF loss per reflection
};

inline float volume (const BoxRoom& r) noexcept { return r.rx * r.ry * r.rz; }

// Sabine's formula: RT60 = 0.161 V / A with A the total equivalent absorption
// area (wall area x damping). Unclamped apart from a division guard — clamp
// to your engine's limits at the call site.
inline float sabineRT60 (const BoxRoom& r) noexcept
{
    const float axy = r.rx * r.ry, axz = r.rx * r.rz, ayz = r.ry * r.rz;
    const float a = ayz * (r.damp[wallXmin] + r.damp[wallXmax])
                  + axz * (r.damp[wallYmin] + r.damp[wallYmax])
                  + axy * (r.damp[wallZmin] + r.damp[wallZmax]);
    return 0.161f * volume (r) / std::max (1.0e-3f, a);
}

// Exponential decay rate k (per sample) such that exp(-k n) falls by 60 dB
// (ln 1000 ~ 6.9078) over rt60 seconds.
inline float t60DecayPerSample (float rt60, double sampleRate) noexcept
{
    return 6.9077553f / std::max (1.0f, rt60 * (float) sampleRate);
}

// One-pole low-pass coefficient for an accumulated high-frequency damping.
// hfSum is the sum of the hfDamp of every wall hit; the cutoff starts at
// refFreq (transparent) and drops as exp(-hfSum).
// Filter form: y[n] = alpha x[n] + (1 - alpha) y[n-1], alpha ~ 1 = no filter.
inline float hfDampingAlpha (float hfSum, double sampleRate,
                             float refFreq = 20000.0f) noexcept
{
    const float omega = 2.0f * 3.14159265f * refFreq * std::exp (-hfSum);
    return std::min (1.0f, 1.0f - std::exp (-omega / (float) sampleRate));
}

//==============================================================================
// One image source, ready for a multitap delay renderer.
struct ImageTap
{
    int   delaySamples = 0;      // integer arrival time
    float gain         = 1.0f;   // broadband: reflection losses x 1/distance
    float lpAlpha      = 1.0f;   // one-pole HF coefficient (1 = transparent)
    bool  direct       = false;  // the order-0 image (direct sound)
    ambi::Vec3 direction { 1.0f, 0.0f, 0.0f };  // unit, room frame, from listener

    // Stable identity of this image across recomputes: a packing of its
    // (ix, iy, iz) reflection-grid indices. The same physical image keeps the
    // same id when the source, listener or room moves, which lets a renderer
    // match old and new tap sets and interpolate per tap (Doppler-style
    // continuous movement) instead of cross-fading whole sets.
    std::int32_t id = 0;
};

struct ImageSourceConfig
{
    double sampleRate    = 48000.0;
    int    maxTaps       = 256;      // keep the earliest-arriving N images
    float  speedOfSound  = 340.0f;   // m/s
    float  minGain       = 1.0e-5f;  // skip images quieter than this (the
                                     // direct tap is always kept)
    float  minDistance   = 0.1f;     // clamp for near-coincident positions
    float  hfRefFreq     = 20000.0f; // hfDampingAlpha reference frequency
};

// Enumerate the image sources of one listener/source pair in a box room,
// sorted by arrival time and truncated to cfg.maxTaps. Positions are in
// metres inside the box. `out` is cleared and reused (no reallocation churn
// across recomputes once it has grown). If `progress` is non-null it is
// updated in [0, 1] while enumerating, so a UI meter can follow along.
//
// This is CPU-bound work intended for a background thread, never the audio
// thread.
inline void computeImageSources (const BoxRoom& room,
                                 ambi::Vec3 listener, ambi::Vec3 source,
                                 const ImageSourceConfig& cfg,
                                 std::vector<ImageTap>& out,
                                 std::atomic<float>* progress = nullptr)
{
    out.clear();

    // Enumeration radius: large enough that the requested number of
    // earliest-arriving taps are all present after truncation, but bounded.
    const int target = std::max (1, cfg.maxTaps);
    int N = (int) std::ceil (std::cbrt ((double) (2 * target)) * 0.5) + 1;
    N = std::min (N, 10);

    for (int ix = -N; ix <= N; ++ix)
    {
        const float cx = std::ceil ((float) ix * 0.5f);
        const float x  = 2.0f * cx * room.rx + ((ix & 1) ? -source.x : source.x);

        for (int iy = -N; iy <= N; ++iy)
        {
            const float cy = std::ceil ((float) iy * 0.5f);
            const float y  = 2.0f * cy * room.ry + ((iy & 1) ? -source.y : source.y);

            for (int iz = -N; iz <= N; ++iz)
            {
                const float cz = std::ceil ((float) iz * 0.5f);
                const float z  = 2.0f * cz * room.rz + ((iz & 1) ? -source.z : source.z);

                const float dx = x - listener.x, dy = y - listener.y, dz = z - listener.z;
                float dist = std::sqrt (dx * dx + dy * dy + dz * dz);
                dist = std::max (dist, cfg.minDistance);

                ImageTap tap;
                tap.direct = (ix == 0 && iy == 0 && iz == 0);
                tap.id = (std::int32_t) ((ix + 16) | ((iy + 16) << 6) | ((iz + 16) << 12));
                tap.delaySamples = (int) std::lround (dist / cfg.speedOfSound
                                                      * (float) cfg.sampleRate);

                // Per-wall reflection counts. Reflections alternate between the
                // two walls of each axis; the first wall hit depends on the
                // travel direction (sign of the image index).
                const int bx = std::abs (ix), by = std::abs (iy), bz = std::abs (iz);
                const int xMax = (ix > 0) ? (bx + 1) / 2 : bx / 2;
                const int xMin = bx - xMax;
                const int yMax = (iy > 0) ? (by + 1) / 2 : by / 2;
                const int yMin = by - yMax;
                const int zMax = (iz > 0) ? (bz + 1) / 2 : bz / 2;
                const int zMin = bz - zMax;

                auto absorb = [] (float damp, int count)
                {
                    return std::pow (std::max (0.0f, 1.0f - damp), (float) count);
                };

                const float reflGain =
                      absorb (room.damp[wallXmin], xMin) * absorb (room.damp[wallXmax], xMax)
                    * absorb (room.damp[wallYmin], yMin) * absorb (room.damp[wallYmax], yMax)
                    * absorb (room.damp[wallZmin], zMin) * absorb (room.damp[wallZmax], zMax);

                const float hfSum =
                      room.hfDamp[wallXmin] * (float) xMin + room.hfDamp[wallXmax] * (float) xMax
                    + room.hfDamp[wallYmin] * (float) yMin + room.hfDamp[wallYmax] * (float) yMax
                    + room.hfDamp[wallZmin] * (float) zMin + room.hfDamp[wallZmax] * (float) zMax;

                tap.lpAlpha = hfDampingAlpha (hfSum, cfg.sampleRate, cfg.hfRefFreq);

                tap.gain = reflGain / dist;
                if (tap.gain < cfg.minGain && ! tap.direct)
                    continue;   // negligible image, skip

                tap.direction = ambi::normalise ({ dx, dy, dz });
                out.push_back (tap);
            }
        }

        // Enumeration over ix is the bulk of the work; reserve the last
        // fraction of the bar for the sort/truncate below.
        if (progress != nullptr)
            progress->store (0.95f * (float) (ix + N + 1) / (float) (2 * N + 1));
    }

    // Keep the earliest-arriving taps (direct sound + nearest reflections).
    std::sort (out.begin(), out.end(),
               [] (const ImageTap& a, const ImageTap& b)
               { return a.delaySamples < b.delaySamples; });

    if ((int) out.size() > target)
        out.resize ((size_t) target);

    if (progress != nullptr)
        progress->store (1.0f);
}

//==============================================================================
// Fade-out window over the tail of a (sorted) tap set, for a smooth hand-over
// to a diffuse late field: the last `fadedFraction` of the taps fade linearly
// to zero, and the diffuse tail should fade *in* over the same absolute-time
// window [startSamples, endSamples].
struct TailCrossfade
{
    int fadeStartIndex = 0;   // first tap whose gain is reduced
    int fadeLength     = 1;   // number of faded taps
    int startSamples   = 0;   // arrival time of the first faded tap
    int endSamples     = 0;   // arrival time of the last kept tap
};

inline TailCrossfade tailCrossfade (const std::vector<ImageTap>& taps,
                                    float fadedFraction = 0.1f)
{
    TailCrossfade w;
    const int count = (int) taps.size();
    if (count == 0)
        return w;

    w.fadeLength     = std::max (1, (int) std::lround ((float) count * fadedFraction));
    w.fadeStartIndex = std::max (0, count - w.fadeLength);
    w.startSamples   = taps[(size_t) w.fadeStartIndex].delaySamples;
    w.endSamples     = taps[(size_t) (count - 1)].delaySamples;
    return w;
}

// Fade factor (1 -> 0) for tap `index` under a TailCrossfade window.
inline float crossfadeGain (const TailCrossfade& w, int index) noexcept
{
    if (index < w.fadeStartIndex)
        return 1.0f;
    return 1.0f - (float) (index - w.fadeStartIndex) / (float) w.fadeLength;
}

} // namespace fxme::room
