/*
  ------------------------------------------------------------------------------
    Homography.h

    Plane projective transform (3x3 homography) between two quadrilaterals,
    solved exactly from 4 point correspondences by direct linear transform
    (an 8x8 Gaussian elimination, no external library).

    The motivating use is camera-to-plane calibration: the user points at the
    4 corners of a physical grid in a camera image, toUnitSquare() maps image
    positions to grid coordinates in [0,1]^2, and the inverse re-projects grid
    lines into the image to visualise the calibration. Any quad-to-quad
    mapping works the same way through fromQuad().

    Conventions:
      - corners are given in the fixed order top-left, top-right,
        bottom-right, bottom-left of the *destination* space; toUnitSquare()
        maps them to (0,0), (1,0), (1,1), (0,1);
      - a homography maps straight lines to straight lines, so projecting a
        segment's endpoints is enough to draw it.

    Degenerate inputs (three collinear corners, coincident points) make the
    solve singular: the factory functions then return nullopt, as does
    inverted() for a non-invertible matrix and apply() when the point falls
    on the horizon line (denominator ~ 0). isConvexQuad() is a cheap UI-side
    sanity check: a non-convex click order still solves, but flips part of
    the plane, which is never what a calibration wants.

    Header-only, no JUCE dependency (std only), so it is testable in a plain
    console app. All double precision.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <array>
#include <cmath>
#include <optional>

namespace fxme
{

class Homography
{
public:
    struct Point
    {
        double x = 0.0, y = 0.0;
    };

    using Quad = std::array<Point, 4>;

    /** Row-major matrix; identity by default. m[2][2] is kept at 1 by the
        solver (the standard 8-degrees-of-freedom normalisation). */
    double m[3][3] { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };

    //==========================================================================
    /** The exact homography taking src[i] to dst[i], or nullopt when the
        correspondences are degenerate. */
    static std::optional<Homography> fromQuad (const Quad& src, const Quad& dst)
    {
        // Each correspondence (x,y) -> (X,Y) gives two rows of an 8x8 system
        // in the unknowns (a..h), with H = [a b c; d e f; g h 1]:
        //   a x + b y + c - g x X - h y X = X
        //   d x + e y + f - g x Y - h y Y = Y
        double a[8][9] {};

        for (int i = 0; i < 4; ++i)
        {
            const double x = src[(size_t) i].x, y = src[(size_t) i].y;
            const double X = dst[(size_t) i].x, Y = dst[(size_t) i].y;

            double* r0 = a[i * 2];
            double* r1 = a[i * 2 + 1];

            r0[0] = x;  r0[1] = y;  r0[2] = 1;  r0[6] = -x * X;  r0[7] = -y * X;  r0[8] = X;
            r1[3] = x;  r1[4] = y;  r1[5] = 1;  r1[6] = -x * Y;  r1[7] = -y * Y;  r1[8] = Y;
        }

        // Gaussian elimination with partial pivoting.
        for (int col = 0; col < 8; ++col)
        {
            int pivot = col;
            for (int row = col + 1; row < 8; ++row)
                if (std::abs (a[row][col]) > std::abs (a[pivot][col]))
                    pivot = row;

            if (std::abs (a[pivot][col]) < 1.0e-12)
                return std::nullopt;                    // singular: degenerate quad

            if (pivot != col)
                for (int k = col; k < 9; ++k)
                    std::swap (a[col][k], a[pivot][k]);

            for (int row = 0; row < 8; ++row)
            {
                if (row == col)
                    continue;

                const double f = a[row][col] / a[col][col];
                for (int k = col; k < 9; ++k)
                    a[row][k] -= f * a[col][k];
            }
        }

        Homography h;
        h.m[0][0] = a[0][8] / a[0][0];  h.m[0][1] = a[1][8] / a[1][1];  h.m[0][2] = a[2][8] / a[2][2];
        h.m[1][0] = a[3][8] / a[3][3];  h.m[1][1] = a[4][8] / a[4][4];  h.m[1][2] = a[5][8] / a[5][5];
        h.m[2][0] = a[6][8] / a[6][6];  h.m[2][1] = a[7][8] / a[7][7];  h.m[2][2] = 1.0;
        return h;
    }

    /** The homography taking the given corners (top-left, top-right,
        bottom-right, bottom-left) to the unit square. This is the
        image-to-plane calibration map. */
    static std::optional<Homography> toUnitSquare (const Quad& corners)
    {
        return fromQuad (corners, Quad { Point { 0, 0 }, Point { 1, 0 },
                                         Point { 1, 1 }, Point { 0, 1 } });
    }

    //==========================================================================
    /** Applies the transform. Nullopt when the point lies on the horizon line
        (projective denominator ~ 0), which cannot happen for points inside a
        calibrated quad. */
    std::optional<Point> apply (Point p) const
    {
        const double w = m[2][0] * p.x + m[2][1] * p.y + m[2][2];

        if (std::abs (w) < 1.0e-12)
            return std::nullopt;

        return Point { (m[0][0] * p.x + m[0][1] * p.y + m[0][2]) / w,
                       (m[1][0] * p.x + m[1][1] * p.y + m[1][2]) / w };
    }

    /** The inverse transform (plane-to-image when this is image-to-plane),
        or nullopt for a non-invertible matrix. */
    std::optional<Homography> inverted() const
    {
        // Adjugate over determinant, then renormalise so m[2][2] == 1.
        const double c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
        const double c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
        const double c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];

        const double det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;

        if (std::abs (det) < 1.0e-12)
            return std::nullopt;

        Homography r;
        r.m[0][0] = c00 / det;
        r.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
        r.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
        r.m[1][0] = c01 / det;
        r.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
        r.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
        r.m[2][0] = c02 / det;
        r.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
        r.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;

        if (std::abs (r.m[2][2]) > 1.0e-12)
        {
            const double s = 1.0 / r.m[2][2];
            for (auto& row : r.m)
                for (auto& v : row)
                    v *= s;
        }

        return r;
    }

    //==========================================================================
    /** True when the 4 points form a strictly convex quad in the given order
        (all cross products of consecutive edges share one sign). The click
        order of a sensible calibration always is. */
    static bool isConvexQuad (const Quad& q)
    {
        int sign = 0;

        for (int i = 0; i < 4; ++i)
        {
            const auto& p0 = q[(size_t) i];
            const auto& p1 = q[(size_t) ((i + 1) % 4)];
            const auto& p2 = q[(size_t) ((i + 2) % 4)];

            const double cross = (p1.x - p0.x) * (p2.y - p1.y)
                               - (p1.y - p0.y) * (p2.x - p1.x);

            if (std::abs (cross) < 1.0e-12)
                return false;                           // collinear corners

            const int s = cross > 0 ? 1 : -1;
            if (sign == 0)
                sign = s;
            else if (s != sign)
                return false;
        }

        return true;
    }
};

} // namespace fxme
