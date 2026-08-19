/*
  ------------------------------------------------------------------------------
    CameraPose.h

    Camera pose from a calibrated plane, and triangulation of a point seen by
    several cameras. The companion to Homography.h: that one answers "where
    on the plane is this pixel", this one answers "where is the camera, and
    how high above the plane is this object".

    The chain is:

      4 clicked grid corners  ->  Homography (image <-> plane)
      + a guess at the lens    ->  CameraPose (rotation, translation)
      + the same object in 2+  ->  triangulate() -> a 3D point

    Everything is expressed in normalised image coordinates ([0,1]^2, origin
    top-left) so nothing here needs pixel dimensions, only the image's aspect
    ratio, which enters through CameraIntrinsics::fromHorizontalFov.

    World coordinates are metric in whatever unit the caller uses for the
    plane size: X across the plane, Y down it, Z up out of it (a right-handed
    frame with the plane at Z = 0). Passing the plane width as 1 gives
    "grid widths" as the unit, which is what a caller with no physical
    measurements wants.

    Accuracy: the pose comes from decomposing the plane homography, so it is
    only as good as the assumed intrinsics (focal length from a stated field
    of view, principal point at the image centre, no distortion). Expect a
    usable height estimate, not a survey. Feeding a properly calibrated
    intrinsic matrix (from a chessboard) improves it directly, which is why
    CameraIntrinsics is a parameter and not baked in.

    Header-only, no JUCE dependency (std only), double precision, so it is
    testable in a plain console app.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include "Homography.h"

#include <array>
#include <cmath>
#include <optional>

namespace fxme
{

//==============================================================================
struct Vec3
{
    double x = 0.0, y = 0.0, z = 0.0;

    Vec3 operator+ (const Vec3& o) const   { return { x + o.x, y + o.y, z + o.z }; }
    Vec3 operator- (const Vec3& o) const   { return { x - o.x, y - o.y, z - o.z }; }
    Vec3 operator* (double s) const        { return { x * s, y * s, z * s }; }

    double dot (const Vec3& o) const       { return x * o.x + y * o.y + z * o.z; }
    double length() const                  { return std::sqrt (dot (*this)); }

    Vec3 cross (const Vec3& o) const
    {
        return { y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x };
    }

    Vec3 normalised() const
    {
        const double n = length();
        return n > 1.0e-12 ? *this * (1.0 / n) : Vec3 {};
    }
};

//==============================================================================
/** Pinhole intrinsics in normalised image coordinates: a point at normalised
    (u, v) has camera-frame direction ((u - cu) / fu, (v - cv) / fv, 1). */
struct CameraIntrinsics
{
    double fu = 1.0, fv = 1.0;    ///< focal length, in image widths / heights
    double cu = 0.5, cv = 0.5;    ///< principal point, normalised

    /** The usual guess when nothing has been measured: a stated horizontal
        field of view, a centred principal point and square pixels.
        `imageAspect` is width / height of the frame. */
    static CameraIntrinsics fromHorizontalFov (double fovRadians, double imageAspect)
    {
        CameraIntrinsics k;
        const double halfFov = 0.5 * fovRadians;

        // f in pixels is (width / 2) / tan(fov / 2); dividing by the width to
        // reach normalised units leaves 1 / (2 tan(fov / 2)).
        k.fu = 1.0 / (2.0 * std::tan (halfFov));
        k.fv = k.fu * imageAspect;      // same focal length, shorter axis
        k.cu = 0.5;
        k.cv = 0.5;
        return k;
    }
};

//==============================================================================
/** A ray in world space. */
struct Ray
{
    Vec3 origin, direction;      ///< direction is expected to be unit length
};

//==============================================================================
/** Rotation and translation taking world coordinates to camera coordinates:
    x_camera = R * X_world + t. */
struct CameraPose
{
    /** Row-major R. Column j is the image of world axis j. */
    double r[3][3] { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    Vec3 t;

    /** The camera's position in world coordinates, -R^T t. */
    Vec3 centre() const
    {
        return { -(r[0][0] * t.x + r[1][0] * t.y + r[2][0] * t.z),
                 -(r[0][1] * t.x + r[1][1] * t.y + r[2][1] * t.z),
                 -(r[0][2] * t.x + r[1][2] * t.y + r[2][2] * t.z) };
    }

    /** Unit direction, in world coordinates, of the ray through a normalised
        image point. */
    Vec3 rayDirection (double u, double v, const CameraIntrinsics& k) const
    {
        const Vec3 inCamera { (u - k.cu) / k.fu, (v - k.cv) / k.fv, 1.0 };

        // World direction is R^T applied to the camera-frame direction.
        return Vec3 { r[0][0] * inCamera.x + r[1][0] * inCamera.y + r[2][0] * inCamera.z,
                      r[0][1] * inCamera.x + r[1][1] * inCamera.y + r[2][1] * inCamera.z,
                      r[0][2] * inCamera.x + r[1][2] * inCamera.y + r[2][2] * inCamera.z }
                   .normalised();
    }

    /** The ray through a normalised image point. */
    Ray rayThrough (double u, double v, const CameraIntrinsics& k) const
    {
        return { centre(), rayDirection (u, v, k) };
    }

    /** Projects a world point back to normalised image coordinates. Nullopt
        when the point is behind the camera. Mostly useful for checking a
        pose, and for drawing. */
    std::optional<Homography::Point> project (const Vec3& world, const CameraIntrinsics& k) const
    {
        const Vec3 c { r[0][0] * world.x + r[0][1] * world.y + r[0][2] * world.z + t.x,
                       r[1][0] * world.x + r[1][1] * world.y + r[1][2] * world.z + t.y,
                       r[2][0] * world.x + r[2][1] * world.y + r[2][2] * world.z + t.z };

        if (c.z <= 1.0e-9)
            return std::nullopt;

        return Homography::Point { k.cu + k.fu * c.x / c.z, k.cv + k.fv * c.y / c.z };
    }

    //==========================================================================
    /** Recovers the pose from the homography that maps the calibrated plane's
        unit square to normalised image coordinates (that is, the inverse of
        Homography::toUnitSquare).

        `planeWidth` and `planeHeight` give the physical size of the grid in
        the caller's chosen world unit; the aspect ratio between them is what
        matters, so passing (1, 1 / aspect) yields world units of one grid
        width.

        Nullopt when the homography is degenerate. */
    static std::optional<CameraPose> fromPlaneHomography (const Homography& unitSquareToImage,
                                                          double planeWidth, double planeHeight,
                                                          const CameraIntrinsics& k)
    {
        if (! (planeWidth > 1.0e-9) || ! (planeHeight > 1.0e-9))
            return std::nullopt;

        // The homography we are given maps unit-square coordinates; the
        // decomposition wants world (metric) ones, so scale the two columns
        // that multiply X and Y.
        double h[3][3];

        for (int row = 0; row < 3; ++row)
        {
            h[row][0] = unitSquareToImage.m[row][0] / planeWidth;
            h[row][1] = unitSquareToImage.m[row][1] / planeHeight;
            h[row][2] = unitSquareToImage.m[row][2];
        }

        // Remove the intrinsics: M = K^-1 H, whose columns are lambda*[r1 r2 t].
        double mtx[3][3];

        for (int col = 0; col < 3; ++col)
        {
            mtx[0][col] = (h[0][col] - k.cu * h[2][col]) / k.fu;
            mtx[1][col] = (h[1][col] - k.cv * h[2][col]) / k.fv;
            mtx[2][col] =  h[2][col];
        }

        Vec3 m1 { mtx[0][0], mtx[1][0], mtx[2][0] };
        Vec3 m2 { mtx[0][1], mtx[1][1], mtx[2][1] };
        Vec3 m3 { mtx[0][2], mtx[1][2], mtx[2][2] };

        const double n1 = m1.length(), n2 = m2.length();

        if (n1 < 1.0e-12 || n2 < 1.0e-12)
            return std::nullopt;

        // The two columns should have equal norm; averaging splits the
        // difference the noise in the clicked corners introduces.
        double scale = 2.0 / (n1 + n2);

        // The plane must be in front of the camera.
        if (m3.z < 0.0)
            scale = -scale;

        Vec3 r1 = m1 * scale;
        Vec3 r2 = m2 * scale;
        const Vec3 translation = m3 * scale;

        // r1 and r2 are only approximately orthonormal; make them so.
        r1 = r1.normalised();
        r2 = (r2 - r1 * r1.dot (r2)).normalised();

        if (r1.length() < 0.5 || r2.length() < 0.5)
            return std::nullopt;

        const Vec3 r3 = r1.cross (r2);

        CameraPose pose;
        pose.r[0][0] = r1.x;  pose.r[0][1] = r2.x;  pose.r[0][2] = r3.x;
        pose.r[1][0] = r1.y;  pose.r[1][1] = r2.y;  pose.r[1][2] = r3.y;
        pose.r[2][0] = r1.z;  pose.r[2][1] = r2.z;  pose.r[2][2] = r3.z;
        pose.t = translation;
        return pose;
    }
};

//==============================================================================
/** Which way "up" is, for a set of cameras looking at a common plane.

    A plane homography cannot tell the plane's two sides apart: the recovered
    Z axis is r1 x r2, so which way it points follows from the order the
    plane's corners were given in, not from the scene. Give the same physical
    grid its corners the other way round and every camera lands at negative
    Z, with objects above the plane reporting negative heights.

    What does resolve it is that cameras able to see the plane are all on the
    same side of it. This returns +1 or -1 such that multiplying a
    triangulated Z by it measures height towards the cameras. In-plane X and
    Y are unaffected, since they are defined by the corners either way.

    Callers that triangulate a point seen by several cameras should apply it
    to the height; ignoring it is the classic way to get a height that is
    correct in magnitude and stuck at zero after clamping. */
inline double heightSign (const CameraPose* poses, int count)
{
    if (poses == nullptr || count <= 0)
        return 1.0;

    double sum = 0.0;

    for (int i = 0; i < count; ++i)
        sum += poses[i].centre().z;

    return sum < 0.0 ? -1.0 : 1.0;
}

//==============================================================================
/** The world point closest to a set of rays, in the least-squares sense
    (the classic "mid-point" solution: minimise the summed squared distance
    to every ray).

    Needs at least two rays, and nullopt when they are parallel or so nearly
    so that the intersection is not determined. */
inline std::optional<Vec3> triangulate (const Ray* rays, int count)
{
    if (rays == nullptr || count < 2)
        return std::nullopt;

    // Sum of (I - d d^T) and of (I - d d^T) * origin.
    double a[3][3] {};
    double b[3] {};

    for (int i = 0; i < count; ++i)
    {
        const Vec3 d = rays[i].direction.normalised();
        const Vec3 o = rays[i].origin;

        const double p[3][3] { { 1.0 - d.x * d.x,     - d.x * d.y,     - d.x * d.z },
                               {     - d.y * d.x, 1.0 - d.y * d.y,     - d.y * d.z },
                               {     - d.z * d.x,     - d.z * d.y, 1.0 - d.z * d.z } };

        const double origin[3] { o.x, o.y, o.z };

        for (int row = 0; row < 3; ++row)
        {
            for (int col = 0; col < 3; ++col)
            {
                a[row][col] += p[row][col];
                b[row]      += p[row][col] * origin[col];
            }
        }
    }

    // Solve the 3x3 system by Gaussian elimination with partial pivoting.
    double aug[3][4] { { a[0][0], a[0][1], a[0][2], b[0] },
                       { a[1][0], a[1][1], a[1][2], b[1] },
                       { a[2][0], a[2][1], a[2][2], b[2] } };

    for (int col = 0; col < 3; ++col)
    {
        int pivot = col;

        for (int row = col + 1; row < 3; ++row)
            if (std::abs (aug[row][col]) > std::abs (aug[pivot][col]))
                pivot = row;

        if (std::abs (aug[pivot][col]) < 1.0e-10)
            return std::nullopt;            // rays parallel: nothing determined

        if (pivot != col)
            for (int k = col; k < 4; ++k)
                std::swap (aug[col][k], aug[pivot][k]);

        for (int row = 0; row < 3; ++row)
        {
            if (row == col)
                continue;

            const double f = aug[row][col] / aug[col][col];

            for (int k = col; k < 4; ++k)
                aug[row][k] -= f * aug[col][k];
        }
    }

    return Vec3 { aug[0][3] / aug[0][0], aug[1][3] / aug[1][1], aug[2][3] / aug[2][2] };
}

} // namespace fxme
