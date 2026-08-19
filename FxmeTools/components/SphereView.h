/*
  ------------------------------------------------------------------------------
    SphereView.h

    The pieces shared by the little orthographic 3D scenes these plugins draw
    around a listening point: an orbit camera, a way to stroke a curve that
    lives on the unit sphere, and the directivity lobe of a first-order
    microphone.

    Scene composition stays with the caller. Which rings to draw, in what
    colours, what is highlighted under the mouse, what is clickable and how the
    hit-testing works are all per-plugin decisions, and trying to share them
    produces a component with a parameter for everything. What is shared here
    is only the geometry, which is identical everywhere and easy to get subtly
    wrong.

    Camera convention, which is the part worth reading before using this:

      * Room frame is the ambisonic one, x front, y left, z up.
      * yaw turns the camera about the world up axis, pitch tips it.
      * Screen x grows with the room's +y and screen y grows downwards, so with
        the camera placed BEHIND the listener (yaw near pi) the scene is not
        mirrored: what is in front of the listener is away from the viewer,
        their left is on the left of the screen and their right on the right.
        A camera in front of the listener shows all three reversed. Mirroring
        the screen axes instead would flip the scene's handedness, so move the
        camera rather than the projection.
      * project() can also report depth, growing towards the viewer, which is
        what a caller needs to paint a scene back to front.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>

#include <FxmeTools/dsp/Ambisonics.h>

namespace fxme
{

/** Orthographic orbit camera over a unit-sphere scene. See the file note for
    the axis convention. */
class SphereProjection
{
public:
    /** Where the centre of the scene lands, and how many pixels one unit of
        the room frame is worth. Call from resized() or from paint(). */
    void setViewport (juce::Point<float> centreInPixels, float pixelsPerUnit) noexcept
    {
        centre = centreInPixels;
        scale  = pixelsPerUnit;
    }

    /** Convenience: centre the scene in `bounds` and fit the unit sphere into
        `fillFraction` of its smaller side. */
    void setViewport (juce::Rectangle<float> bounds, float fillFraction = 0.40f) noexcept
    {
        setViewport (bounds.getCentre(),
                     fillFraction * juce::jmin (bounds.getWidth(), bounds.getHeight()));
    }

    void setCamera (float yawRadians, float pitchRadians) noexcept
    {
        yawAngle   = yawRadians;
        pitchAngle = juce::jlimit (-pitchLimit, pitchLimit, pitchRadians);
    }

    /** Drag handler: add to the current angles, with the pitch clamped so the
        camera never tips past the poles and turns the scene upside down. */
    void orbit (float deltaYaw, float deltaPitch) noexcept
    {
        setCamera (yawAngle + deltaYaw, pitchAngle + deltaPitch);
    }

    float yaw()   const noexcept { return yawAngle; }
    float pitch() const noexcept { return pitchAngle; }

    juce::Point<float> project (ambi::Vec3 p) const noexcept
    {
        float ignored = 0.0f;
        return project (p, ignored);
    }

    /** `depth` grows towards the viewer, for back-to-front painting. */
    juce::Point<float> project (ambi::Vec3 p, float& depth) const noexcept
    {
        const float ca = std::cos (yawAngle),   sa = std::sin (yawAngle);
        const float cb = std::cos (pitchAngle), sb = std::sin (pitchAngle);

        // Yaw about z, then pitch about the screen-horizontal axis; view along x''.
        const float x1 =  ca * p.x + sa * p.y;
        const float y1 = -sa * p.x + ca * p.y;
        const float x2 =  cb * x1 + sb * p.z;
        const float z2 = -sb * x1 + cb * p.z;

        depth = x2;
        return { centre.x + y1 * scale, centre.y - z2 * scale };
    }

    /** How far the camera may tip from the horizon, a hair short of the pole. */
    static constexpr float pitchLimit = 1.45f;

private:
    juce::Point<float> centre { 0.0f, 0.0f };
    float scale      = 100.0f;
    float yawAngle   = 0.0f;
    float pitchAngle = 0.0f;
};

//==============================================================================
/** Projected path of a closed curve given as a function of a parameter running
    once around [0, 2pi): great circles, spread-cap outlines, lobe meridians.

    `directionAt` is any callable taking a float and returning an ambi::Vec3,
    so it can carry whatever radius or rotation the caller wants. The path is
    left open; call closeSubPath() on it if the curve should be filled. */
template <typename DirectionAt>
juce::Path sphereCurvePath (const SphereProjection& projection,
                            DirectionAt&& directionAt,
                            int numSegments = 72)
{
    juce::Path path;
    const int steps = juce::jmax (3, numSegments);

    for (int i = 0; i <= steps; ++i)
    {
        const float t = juce::MathConstants<float>::twoPi * (float) i / (float) steps;
        const auto  q = projection.project (directionAt (t));

        if (i == 0) path.startNewSubPath (q);
        else        path.lineTo (q);
    }

    return path;
}

/** A great circle of the given radius in the plane spanned by two axes. The
    three obvious calls are (1,0,0)/(0,1,0) for the horizon, (1,0,0)/(0,0,1)
    for the median plane and (0,1,0)/(0,0,1) for the frontal one. */
inline juce::Path sphereGreatCirclePath (const SphereProjection& projection,
                                         ambi::Vec3 axisA, ambi::Vec3 axisB,
                                         float radius = 1.0f, int numSegments = 72)
{
    return sphereCurvePath (projection,
                            [=] (float t) -> ambi::Vec3
                            {
                                const float c = radius * std::cos (t);
                                const float s = radius * std::sin (t);
                                return { axisA.x * c + axisB.x * s,
                                         axisA.y * c + axisB.y * s,
                                         axisA.z * c + axisB.z * s };
                            },
                            numSegments);
}

//==============================================================================
/** How a directivity lobe is drawn. Everything here is a look, not geometry,
    so a caller can highlight a hovered microphone by passing a different one. */
struct MicLobeStyle
{
    juce::Colour colour       { juce::Colours::white };
    float fillAlpha           = 0.07f;   // multiplied into `colour`
    float strokeAlpha         = 0.50f;
    float strokeWidth         = 1.0f;
    float radius              = 0.55f;   // lobe size at unit gain, in room units
    int   numMeridians        = 4;       // half-planes swept around the axis
    int   segmentsPerMeridian = 48;
};

/** Paints the first-order directivity lobe of a microphone pointing along
    `axis`, as a set of meridian curves swept around that axis.

    A first-order polar pattern is a solid of revolution about its own axis, so
    every plane through the axis gives the same 2D lobe outline; drawing a
    handful of them reads as a 3D shape without needing a mesh. `patternAlpha`
    is the usual first-order coefficient, i.e. fxme::ambi::micPatternAlpha().

    The lobe is the *magnitude* of the pattern, so the rear part of a figure-8
    is drawn as a lobe rather than as nothing. */
inline void paintMicLobe (juce::Graphics& g,
                          const SphereProjection& projection,
                          ambi::Vec3 axis,
                          float patternAlpha,
                          const MicLobeStyle& style)
{
    // Orthonormal frame transverse to the axis, to sweep the meridians around
    // it. The fallback covers an axis parallel to world up, where the cross
    // product degenerates.
    ambi::Vec3 u = ambi::cross (axis, { 0.0f, 0.0f, 1.0f });
    if (u.x * u.x + u.y * u.y + u.z * u.z < 1.0e-6f)
        u = { 0.0f, 1.0f, 0.0f };
    u = ambi::normalise (u);
    const ambi::Vec3 v = ambi::normalise (ambi::cross (axis, u));

    const int meridians = juce::jmax (1, style.numMeridians);

    for (int k = 0; k < meridians; ++k)
    {
        const float phi = juce::MathConstants<float>::pi * (float) k / (float) meridians;
        const float cp = std::cos (phi), sp = std::sin (phi);
        const ambi::Vec3 p { u.x * cp + v.x * sp,
                             u.y * cp + v.y * sp,
                             u.z * cp + v.z * sp };

        auto lobe = sphereCurvePath (projection,
                                     [&] (float t) -> ambi::Vec3
                                     {
                                         const float ct = std::cos (t), st = std::sin (t);
                                         const ambi::Vec3 d { axis.x * ct + p.x * st,
                                                              axis.y * ct + p.y * st,
                                                              axis.z * ct + p.z * st };
                                         const float gain = std::abs (ambi::micGain (patternAlpha, axis, d));
                                         return { style.radius * gain * d.x,
                                                  style.radius * gain * d.y,
                                                  style.radius * gain * d.z };
                                     },
                                     style.segmentsPerMeridian);
        lobe.closeSubPath();

        g.setColour (style.colour.withAlpha (style.fillAlpha));
        g.fillPath (lobe);
        g.setColour (style.colour.withAlpha (style.strokeAlpha));
        g.strokePath (lobe, juce::PathStrokeType (style.strokeWidth));
    }
}

} // namespace fxme
