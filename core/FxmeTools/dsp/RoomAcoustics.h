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
      energyAbsorption (damp)      alpha = 1 - (1-damp)^2. `damp` is an
                                   AMPLITUDE loss, so this converts it to the
                                   energy coefficient the RT60 formulae want.
      sabineRT60 (room)            RT60 = 0.161 V / A, A = sum of area x alpha.
      eyringRT60 (room)            RT60 = 0.161 V / A, A = -sum(S ln(1-alpha))
                                   = -2 sum(S ln(1-damp)). Correct at high
                                   absorption too; use this one to match an
                                   image-source early field.
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

// Energy absorption coefficient of a wall. `damp` is an *amplitude* loss (the
// image-source model reflects with the factor 1 - damp), so the fraction of
// *energy* absorbed per reflection is alpha = 1 - (1 - damp)^2. Feeding `damp`
// straight into Sabine's formula, as if it were already an energy coefficient,
// makes the predicted RT60 far too long — the tail then decays much more
// slowly than the image-source field it is supposed to continue.
inline float energyAbsorption (float damp) noexcept
{
    const float refl = 1.0f - damp;             // amplitude reflectance
    return 1.0f - refl * refl;
}

// Sum over the six walls of (wall area) x f(damp), for a per-wall weight f.
template <typename WallWeight>
inline float weightedWallArea (const BoxRoom& r, WallWeight f) noexcept
{
    const float axy = r.rx * r.ry, axz = r.rx * r.rz, ayz = r.ry * r.rz;
    return ayz * (f (r.damp[wallXmin]) + f (r.damp[wallXmax]))
         + axz * (f (r.damp[wallYmin]) + f (r.damp[wallYmax]))
         + axy * (f (r.damp[wallZmin]) + f (r.damp[wallZmax]));
}

// Sabine's formula: RT60 = 0.161 V / A, with A the equivalent absorption area
// (wall area x *energy* absorption). Accurate for small alpha; it overestimates
// RT60 as the room gets more absorbent. Unclamped apart from a division guard.
inline float sabineRT60 (const BoxRoom& r) noexcept
{
    const float a = weightedWallArea (r, energyAbsorption);
    return 0.161f * volume (r) / std::max (1.0e-3f, a);
}

// Eyring/Norris: RT60 = 0.161 V / A with A = -sum(S_w * ln(1 - alpha_w)).
// Since alpha = 1 - (1-damp)^2, the log collapses neatly:
//     -ln(1 - alpha) = -ln((1-damp)^2) = -2 ln(1 - damp)
// Valid across the whole absorption range, so this is the one to use when the
// tail must line up with an image-source early field (which decays exactly as
// (1-damp)^(2n) in energy).
inline float eyringRT60 (const BoxRoom& r) noexcept
{
    auto weight = [] (float damp)
    {
        const float refl = std::max (1.0e-3f, 1.0f - damp);   // amplitude reflectance
        return -2.0f * std::log (refl);
    };
    const float a = weightedWallArea (r, weight);
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
    int    maxTaps       = 256;      // hard cap on the number of images kept
    float  speedOfSound  = 340.0f;   // m/s
    float  minGain       = 1.0e-5f;  // skip images quieter than this (the
                                     // direct tap is always kept)
    float  minDistance   = 0.1f;     // clamp for near-coincident positions
    float  hfRefFreq     = 20000.0f; // hfDampingAlpha reference frequency

    // Truncation policy.
    //
    // maxDelaySamples == 0 (default): keep the `maxTaps` earliest images. The
    // arrival time of the last one then depends on where the source and the
    // listener stand, which is fine for a renderer that follows them.
    //
    // maxDelaySamples > 0: keep every image arriving no later than this, and
    // cap the count at `maxTaps` only as a safety net. The enumeration radius
    // is then derived per axis from the time bound, so flat and tall rooms are
    // covered correctly — a fixed index box is not, since the image lattice
    // has the room's aspect ratio. Use this when the truncation time must be
    // position-independent (e.g. it has to line up with a shared, static
    // late-reverb impulse response).
    int    maxDelaySamples = 0;
};

// Linear amplitude fade-out for the early taps across an absolute-time window:
// 1 before crossStart, 0 at and after crossEnd. Pair it with a diffuse fade-in
// of sqrt(w(2-w)) over the same window (w = the normalised position in it):
// the two are then power-complementary, so an uncorrelated early field and
// diffuse tail sum to unit energy at every instant, whatever the geometry.
inline float timeCrossfadeGain (int crossStart, int crossEnd, int delaySamples) noexcept
{
    if (delaySamples <= crossStart) return 1.0f;
    if (delaySamples >= crossEnd)   return 0.0f;
    return 1.0f - (float) (delaySamples - crossStart)
                / (float) std::max (1, crossEnd - crossStart);
}

// Enumerate the image sources of one listener/source pair in a box room,
// sorted by arrival time and truncated per cfg (see ImageSourceConfig).
// Positions are in metres inside the box. `out` is cleared and reused (no
// reallocation churn across recomputes once it has grown). If `progress` is
// non-null it is updated in [0, 1] while enumerating, so a UI meter can
// follow along.
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

    const int target = std::max (1, cfg.maxTaps);
    constexpr int maxAxisIndex = 64;      // guard against absurd enumerations

    // Enumeration radius, per axis. Always derived from a *radius* in metres
    // and converted per axis, because the image lattice inherits the room's
    // aspect ratio: a cube of indices under-samples the thin axis of a flat or
    // tall room, so the n-th earliest arrival comes out far too late.
    auto stepsForRadius = [&] (float r)
    {
        auto steps = [&] (float dim)
        {
            return std::min (maxAxisIndex,
                             (int) std::ceil (r / std::max (0.01f, dim)) + 1);
        };
        return std::array<int, 3> { steps (room.rx), steps (room.ry), steps (room.rz) };
    };

    std::array<int, 3> Nxyz;
    if (cfg.maxDelaySamples > 0)
    {
        // Time-bounded: cover the sphere of radius r = c * t exactly.
        Nxyz = stepsForRadius ((float) cfg.maxDelaySamples / (float) cfg.sampleRate
                               * cfg.speedOfSound);
    }
    else
    {
        // Count-bounded: the sphere holding `target` images has radius
        // r = cbrt(3 n V / 4pi) (image density is 1/V). Enumerate a little
        // beyond it so the truncation to `target` is exact even where the
        // culling below removes some images.
        const float V = std::max (1.0e-3f, volume (room));
        const float r = std::cbrt (3.0f * (float) target * V / (4.0f * 3.14159265f));
        Nxyz = stepsForRadius (1.35f * r);
    }
    const int Nx = Nxyz[0], Ny = Nxyz[1], Nz = Nxyz[2];
    const int N = Nx;   // for the progress fraction below

    for (int ix = -Nx; ix <= Nx; ++ix)
    {
        const float cx = std::ceil ((float) ix * 0.5f);
        const float x  = 2.0f * cx * room.rx + ((ix & 1) ? -source.x : source.x);

        for (int iy = -Ny; iy <= Ny; ++iy)
        {
            const float cy = std::ceil ((float) iy * 0.5f);
            const float y  = 2.0f * cy * room.ry + ((iy & 1) ? -source.y : source.y);

            for (int iz = -Nz; iz <= Nz; ++iz)
            {
                const float cz = std::ceil ((float) iz * 0.5f);
                const float z  = 2.0f * cz * room.rz + ((iz & 1) ? -source.z : source.z);

                const float dx = x - listener.x, dy = y - listener.y, dz = z - listener.z;
                float dist = std::sqrt (dx * dx + dy * dy + dz * dz);
                dist = std::max (dist, cfg.minDistance);

                ImageTap tap;
                tap.direct = (ix == 0 && iy == 0 && iz == 0);
                // 8 bits per axis (offset 128): the per-axis enumeration radius
                // can reach 64, which a 6-bit field would silently wrap.
                tap.id = (std::int32_t) ((ix + 128) | ((iy + 128) << 8) | ((iz + 128) << 16));
                tap.delaySamples = (int) std::lround (dist / cfg.speedOfSound
                                                      * (float) cfg.sampleRate);

                // Time-bounded truncation: drop late images before doing any of
                // the expensive per-wall work below.
                if (cfg.maxDelaySamples > 0 && tap.delaySamples > cfg.maxDelaySamples)
                    continue;

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
