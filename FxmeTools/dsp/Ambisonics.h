/*
  ------------------------------------------------------------------------------
    Ambisonics.h

    Ambisonic helpers shared across FX-Mechanics projects, in the AmbiX
    convention: ACN channel ordering, SN3D normalisation, up to 3rd order
    (16 channels). Header-only and JUCE-free — depends only on <cmath> /
    <array> — so it can be reused from any C++ project.

    Conventions
    -----------
      * Coordinate frame: right-handed, x = front, y = left, z = up (the
        standard acoustics / ambisonics frame).
      * Azimuth  = yaw, radians, counter-clockwise about +z, 0 at +x.
      * Elevation = pitch, radians, positive above the horizon.
      * Roll = right-handed rotation about the forward axis, radians.
      * Channel index: acn = l·(l+1) + m for degree l and order m.
      * SN3D normalisation (AmbiX). Note that per-degree rotations are
        identical in SN3D and N3D, so ShRotation works for both.

    API summary
    -----------
      channelsForOrder (l)         (l+1)^2 channels for ambisonic order l.
      orderOfChannel (acn)         Degree l of an ACN channel index.
      diffuseFieldOrderGain (l)    1/sqrt(2l+1): per-degree weight giving an
                                   isotropic (energy-balanced) diffuse field.
      Vec3, normalise()            Minimal 3-vector.
      capDirection (k, n, angle)   k-th of n quasi-uniform directions in the
                                   spherical cap of given half-angle around +x
                                   (Fibonacci spiral; angle = pi covers the
                                   whole sphere).
      encodeSN3D (dir, gains, order)
                                   Real spherical-harmonic encoding gains for a
                                   unit direction, ACN/SN3D, up to order 3.
      Mat3                         3x3 rotation matrix (row-major, v' = M·v).
      headRotation (az, el, roll)  Rotation mapping room-frame directions into
                                   the head frame of a listener oriented by
                                   (azimuth, elevation, roll).
      ShRotation                   Real-SH rotation matrix up to order 3, built
                                   from a Mat3 with the Ivanic–Ruedenberg
                                   recurrence. Block-diagonal per degree.
                                   Guarantee:
                                     encodeSN3D(M·d) == ShRotation::fromMatrix(M)
                                                          .apply(encodeSN3D(d))

    Typical use: head-tracked / rotatable playback
    ----------------------------------------------
    Encode (or compute reverb taps) once in the *room* frame, then rotate the
    ambisonic stream in realtime when the listener turns — no re-encoding:

        // background / setup: room-frame encoding
        float g[fxme::ambi::maxChannels];
        fxme::ambi::encodeSN3D (direction, g);

        // message thread, when azimuth / elevation change (cheap):
        auto rot = fxme::ambi::ShRotation::fromMatrix (
                       fxme::ambi::headRotation (azimuth, elevation));

        // audio thread, per sample frame (in != out):
        rot.apply (in, out, numChannels);

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <cmath>
#include <array>

namespace fxme::ambi
{

inline constexpr int maxOrder    = 3;
inline constexpr int maxChannels = (maxOrder + 1) * (maxOrder + 1);   // 16

// Channel count for a given ambisonic order.
inline constexpr int channelsForOrder (int order) noexcept
{
    return (order + 1) * (order + 1);
}

// Degree l of an ACN channel index (0 -> 0, 1..3 -> 1, 4..8 -> 2, 9..15 -> 3).
inline int orderOfChannel (int acn) noexcept
{
    return (int) std::sqrt ((double) acn);
}

// Per-degree weight (1/sqrt(2l+1)) for an isotropic diffuse field in SN3D.
inline float diffuseFieldOrderGain (int order) noexcept
{
    return 1.0f / std::sqrt ((float) (2 * order + 1));
}

//==============================================================================
struct Vec3
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

inline Vec3 normalise (Vec3 v) noexcept
{
    const float n = std::sqrt (v.x * v.x + v.y * v.y + v.z * v.z);
    if (n < 1.0e-9f)
        return { 1.0f, 0.0f, 0.0f };   // arbitrary front for a degenerate vector
    const float inv = 1.0f / n;
    return { v.x * inv, v.y * inv, v.z * inv };
}

//==============================================================================
// k-th of n quasi-uniform directions (by solid angle) inside the spherical cap
// of half-angle `capHalfAngle` (radians) centred on +x, i.e. the front.
// Points follow a Fibonacci spiral: deterministic, well spread for any n, and
// a given point moves continuously when the cap opens or closes. A half-angle
// of pi covers the whole sphere.
inline Vec3 capDirection (int k, int n, float capHalfAngle) noexcept
{
    constexpr float goldenAngle = 2.3999632297f;   // pi * (3 - sqrt 5)

    const float w        = ((float) k + 0.5f) / (float) (n > 0 ? n : 1);
    const float cosTheta = 1.0f - w * (1.0f - std::cos (capHalfAngle));
    const float sinTheta = std::sqrt (std::fmax (0.0f, 1.0f - cosTheta * cosTheta));
    const float azimuth  = (float) k * goldenAngle;

    return { cosTheta, sinTheta * std::cos (azimuth), sinTheta * std::sin (azimuth) };
}

//==============================================================================
// Fill `gains` with the ACN/SN3D real spherical-harmonic encoding coefficients
// for a *unit* direction (x = front, y = left, z = up). Writes
// channelsForOrder(order) values; the W channel (ACN 0) is 1.
inline void encodeSN3D (Vec3 d, float* gains, int order = maxOrder) noexcept
{
    const float x = d.x, y = d.y, z = d.z;
    const float xx = x * x, yy = y * y, zz = z * z;

    // Order 0
    gains[0]  = 1.0f;                                   // ACN0  W
    if (order < 1) return;

    // Order 1
    gains[1]  = y;                                      // ACN1  Y
    gains[2]  = z;                                      // ACN2  Z
    gains[3]  = x;                                      // ACN3  X
    if (order < 2) return;

    // Order 2
    gains[4]  = 1.7320508f * x * y;                     // ACN4  V  (sqrt 3)
    gains[5]  = 1.7320508f * y * z;                     // ACN5  T
    gains[6]  = 0.5f * (3.0f * zz - 1.0f);              // ACN6  R
    gains[7]  = 1.7320508f * x * z;                     // ACN7  S
    gains[8]  = 0.8660254f * (xx - yy);                 // ACN8  U  (sqrt3 / 2)
    if (order < 3) return;

    // Order 3
    gains[9]  = 0.7905694f * y * (3.0f * xx - yy);      // ACN9  Q  (sqrt 5/8)
    gains[10] = 3.8729833f * x * y * z;                 // ACN10 O  (sqrt 15)
    gains[11] = 0.6123724f * y * (5.0f * zz - 1.0f);    // ACN11 M  (sqrt 3/8)
    gains[12] = 0.5f * z * (5.0f * zz - 3.0f);          // ACN12 K
    gains[13] = 0.6123724f * x * (5.0f * zz - 1.0f);    // ACN13 L  (sqrt 3/8)
    gains[14] = 1.9364917f * z * (xx - yy);             // ACN14 N  (sqrt 15 / 2)
    gains[15] = 0.7905694f * x * (xx - 3.0f * yy);      // ACN15 P  (sqrt 5/8)
}

//==============================================================================
// Small row-major 3x3 rotation matrix, applied as v' = M · v.
struct Mat3
{
    float m[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };

    Vec3 operator* (Vec3 v) const noexcept
    {
        return { m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
                 m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                 m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z };
    }

    Mat3 transposed() const noexcept
    {
        Mat3 t;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                t.m[i][j] = m[j][i];
        return t;
    }
};

// Rotation mapping room-frame directions into the head frame of a listener
// oriented by (azimuth, elevation, roll): dHead = headRotation(...) · dRoom.
// Its rows are the head's forward / left / up axes in room coordinates.
// Feed it to ShRotation::fromMatrix() to rotate a room-frame ambisonic stream
// into the listener's frame.
inline Mat3 headRotation (float azimuth, float elevation, float roll = 0.0f) noexcept
{
    const float ca = std::cos (azimuth),   sa = std::sin (azimuth);
    const float ce = std::cos (elevation), se = std::sin (elevation);

    // Head axes in room coordinates, before roll.
    const Vec3 fwd  { ce * ca,  ce * sa,  se };
    const Vec3 left { -sa,      ca,       0.0f };
    const Vec3 up   { -se * ca, -se * sa, ce };

    // Roll: right-handed rotation of the (left, up) pair about `fwd`.
    const float cr = std::cos (roll), sr = std::sin (roll);
    const Vec3 leftR { cr * left.x + sr * up.x, cr * left.y + sr * up.y, cr * left.z + sr * up.z };
    const Vec3 upR   { cr * up.x - sr * left.x, cr * up.y - sr * left.y, cr * up.z - sr * left.z };

    Mat3 r;
    r.m[0][0] = fwd.x;   r.m[0][1] = fwd.y;   r.m[0][2] = fwd.z;
    r.m[1][0] = leftR.x; r.m[1][1] = leftR.y; r.m[1][2] = leftR.z;
    r.m[2][0] = upR.x;   r.m[2][1] = upR.y;   r.m[2][2] = upR.z;
    return r;
}

//==============================================================================
// Real spherical-harmonic rotation matrix up to order 3 (ACN ordering, valid
// for SN3D and N3D alike). Built from a 3x3 rotation with the Ivanic &
// Ruedenberg recurrence (J. Phys. Chem. 1996, corrected 1998). The matrix is
// block-diagonal per degree, and apply() exploits that structure, so rotating
// a full 16-channel frame costs 3x3 + 5x5 + 7x7 = 83 multiplies.
class ShRotation
{
public:
    ShRotation() noexcept { setIdentity(); }

    void setIdentity() noexcept
    {
        for (int i = 0; i < maxChannels; ++i)
            for (int j = 0; j < maxChannels; ++j)
                coeffs[i][j] = (i == j) ? 1.0f : 0.0f;
    }

    // Build the SH rotation equivalent to the 3x3 rotation `rot`:
    //   encodeSN3D (rot · d)  ==  fromMatrix (rot).apply (encodeSN3D (d)).
    static ShRotation fromMatrix (const Mat3& rot) noexcept
    {
        ShRotation r;

        // Working blocks indexed [m + l][n + l]; degree 1 comes straight from
        // the 3x3 rotation with the (y, z, x) axis order of ACN degree 1.
        const float b1[3][3] = {
            { rot.m[1][1], rot.m[1][2], rot.m[1][0] },   // Y' row
            { rot.m[2][1], rot.m[2][2], rot.m[2][0] },   // Z' row
            { rot.m[0][1], rot.m[0][2], rot.m[0][0] }    // X' row
        };

        r.setBlock (1, &b1[0][0], 3, 3);

        float prev[7][7] = {};    // block for degree l-1
        float cur[7][7]  = {};    // block for degree l
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                prev[i][j] = b1[i][j];

        // Recurrence per Ivanic & Ruedenberg (J. Phys. Chem. 1996; corrected
        // 1998), in the row-m / column-n arrangement of the corrected paper.
        for (int l = 2; l <= maxOrder; ++l)
        {
            const int size = 2 * l + 1;

            // Degree-1 block by signed indices i, j in {-1, 0, 1}.
            auto R1 = [&b1] (int i, int j) { return b1[i + 1][j + 1]; };
            // Degree-(l-1) block by signed indices.
            auto Rp = [&prev, l] (int a, int b) { return prev[a + l - 1][b + l - 1]; };

            auto P = [&] (int i, int a, int b) -> float
            {
                if (b ==  l) return R1 (i, 1) * Rp (a,  l - 1) - R1 (i, -1) * Rp (a, -l + 1);
                if (b == -l) return R1 (i, 1) * Rp (a, -l + 1) + R1 (i, -1) * Rp (a,  l - 1);
                return R1 (i, 0) * Rp (a, b);
            };

            for (int m = -l; m <= l; ++m)             // row
            {
                const int am = std::abs (m);
                const float d = (m == 0) ? 1.0f : 0.0f;

                for (int n = -l; n <= l; ++n)         // column
                {
                    const float denom = (std::abs (n) == l)
                                            ? (float) ((2 * l) * (2 * l - 1))
                                            : (float) ((l + n) * (l - n));

                    float u = std::sqrt ((float) ((l + m) * (l - m)) / denom);
                    float v = 0.5f * std::sqrt ((1.0f + d) * (float) ((l + am - 1) * (l + am)) / denom)
                                   * (1.0f - 2.0f * d);
                    float w = -0.5f * std::sqrt ((float) ((l - am - 1) * (l - am)) / denom)
                                    * (1.0f - d);

                    if (u != 0.0f)
                        u *= P (0, m, n);

                    if (v != 0.0f)
                    {
                        if (m == 0)
                            v *= P (1, 1, n) + P (-1, -1, n);
                        else if (m > 0)
                            v *= P (1, m - 1, n) * std::sqrt (m == 1 ? 2.0f : 1.0f)
                               - P (-1, -m + 1, n) * (m == 1 ? 0.0f : 1.0f);
                        else
                            v *= P (1, m + 1, n) * (m == -1 ? 0.0f : 1.0f)
                               + P (-1, -m - 1, n) * std::sqrt (m == -1 ? 2.0f : 1.0f);
                    }

                    if (w != 0.0f)
                    {
                        if (m > 0)
                            w *= P (1, m + 1, n) + P (-1, -m - 1, n);
                        else
                            w *= P (1, m - 1, n) - P (-1, -m + 1, n);
                    }

                    cur[m + l][n + l] = u + v + w;
                }
            }

            r.setBlock (l, &cur[0][0], size, 7);
            for (int i = 0; i < size; ++i)
                for (int j = 0; j < size; ++j)
                    prev[i][j] = cur[i][j];
        }

        return r;
    }

    // Rotate one sample frame of `numChannels` ambisonic channels (a whole
    // number of degrees, i.e. 1, 4, 9 or 16 channels). `in` and `out` must not
    // alias. Degrees above the given channel count are left untouched.
    void apply (const float* in, float* out, int numChannels = maxChannels) const noexcept
    {
        out[0] = in[0];                                    // W is invariant
        for (int l = 1; l <= maxOrder; ++l)
        {
            const int base = l * l;
            const int size = 2 * l + 1;
            if (base + size > numChannels)
                break;
            for (int i = 0; i < size; ++i)
            {
                float sum = 0.0f;
                for (int j = 0; j < size; ++j)
                    sum += coeffs[base + i][base + j] * in[base + j];
                out[base + i] = sum;
            }
        }
    }

    float coeff (int row, int col) const noexcept { return coeffs[row][col]; }
    float& coeff (int row, int col) noexcept      { return coeffs[row][col]; }

private:
    void setBlock (int l, const float* block, int size, int stride) noexcept
    {
        const int base = l * l;
        for (int i = 0; i < size; ++i)
            for (int j = 0; j < size; ++j)
                coeffs[base + i][base + j] = block[i * stride + j];
    }

    float coeffs[maxChannels][maxChannels];
};

} // namespace fxme::ambi
