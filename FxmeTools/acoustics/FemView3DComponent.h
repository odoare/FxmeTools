/*
  ------------------------------------------------------------------------------
    FemView3DComponent.h

    JUCE component displaying a FemMesh (FemMesh.h) as a deformed surface in
    three dimensions: the nodal field is the height, the mesh is the sheet,
    and the view orbits under click-and-drag. The companion to
    FemViewComponent, which draws the same mesh and field flat as contours.

    Message-thread only, like any juce::Component. Typical use:

        fxme::acoustics::FemView3DComponent view;
        view.setMesh (meshPtr);                  // shared_ptr<const FemMesh>
        view.setField (result.shapes[k]);        // per-vertex values (or {})
        view.setFieldScale (reference);          // hold one scale across frames

    It is deliberately a *display*: there is no onPlateClick, because the drag
    gesture is the camera. Anything that needs to be pointed at on the plate
    is pointed at in the flat view, where a screen position means one plate
    position; here it does not, since the surface folds over itself.

    Geometry, which is the part worth reading before using this
    ------------------------------------------------------------------------
      * Plate coordinates are the mesh's own. The mesh's bounding box is
        centred and scaled to fit, so a plate that does not sit in the unit
        square draws the same as one that does.
      * Height is the field value times heightScale, expressed as a fraction
        of the fitted plate width — so the relief keeps its proportions when
        the component is resized, and a taller setting means a taller plate
        rather than a bigger picture.
      * `azimuth` turns the plate about its own vertical axis; `elevation` is
        the camera's angle above the plate plane. At elevation = pi/2 the view
        is straight down and identical in outline to the flat one, which makes
        it a useful place to start from when checking that a field looks the
        same in both.
      * The projection is orthographic. A perspective divide would make the
        near edge of a flat plate larger than the far one, which reads as the
        plate being wedge-shaped rather than as depth.
      * The mouse wheel zooms about the centre of the view. There is no pan,
        so zooming well in leaves the edges of the plate off screen; turning
        the plate brings a different part of it under the centre, which is
        the gesture that stands in for one.

    Rendering
    ---------
    Painter's algorithm: triangles are sorted back to front and filled. That
    is the right choice here rather than a depth buffer, because the surface
    is a heightfield with no self-intersection, so a per-triangle ordering is
    exact; and because it needs nothing but juce::Graphics.

    Each facet is flat-shaded: its colour comes from the field at its centroid
    through the same diverging map the flat view uses, lit by a Lambert term
    from the facet normal. The lighting is what makes a deformation legible —
    colour alone leaves a bump and a dip looking alike from directly above.

    The mesh is stroked over each facet as it is filled (setShowGrid, on by
    default), which both gives the eye a grid to read the relief against and
    removes its own hidden lines: a nearer facet drawn later covers the edges
    behind it.

    Cost is one fill per triangle per repaint, software-rendered, so it scales
    with the mesh rather than with the window. A few thousand triangles at
    30 Hz is comfortable; a very fine mesh is better viewed flat.

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <FxmeTools/acoustics/FemMesh.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <numeric>
#include <vector>

namespace fxme::acoustics
{

/** Orthographic orbit camera over a heightfield.

    Separate from the component so that it can be used to project anything
    into the same scene — a marker, an outline, an annotation — with exactly
    the geometry the surface was drawn with. See the file note for the
    convention. */
class HeightFieldProjection
{
public:
    /** Where the centre of the scene lands, and how many pixels one plate
        unit is worth. */
    void setViewport (juce::Point<float> centreInPixels, float pixelsPerUnit) noexcept
    {
        centre = centreInPixels;
        scale = pixelsPerUnit;
    }

    /** azimuth turns the plate about its vertical axis; elevation is the
        camera's angle above the plate plane, clamped to just inside straight
        down and straight along, where the projection degenerates. */
    void setOrientation (float azimuthRadians, float elevationRadians) noexcept
    {
        azimuth = azimuthRadians;
        elevation = juce::jlimit (0.02f, juce::MathConstants<float>::halfPi - 0.02f,
                                  elevationRadians);
    }

    float getAzimuth() const noexcept   { return azimuth; }
    float getElevation() const noexcept { return elevation; }

    /** Screen position of a point given in *centred* plate coordinates
        (the scene's origin at the plate's centre) with height z.
        `depthAway`, when given, receives the distance from the viewer,
        growing away — sort by it descending to paint back to front. */
    juce::Point<float> project (float x, float y, float z,
                                float* depthAway = nullptr) const noexcept
    {
        const float ca = std::cos (azimuth),  sa = std::sin (azimuth);
        const float ce = std::cos (elevation), se = std::sin (elevation);

        const float xa =  x * ca + y * sa;      // turn about the vertical
        const float ya = -x * sa + y * ca;

        // Straight down (elevation = pi/2) leaves ya as the screen's vertical
        // and z invisible; edge-on leaves z as the vertical. Tilting mixes
        // them, and the complement mixes into depth.
        const float up = z * ce + ya * se;
        if (depthAway != nullptr)
            *depthAway = ya * ce - z * se;

        return { centre.x + scale * xa, centre.y - scale * up };
    }

private:
    juce::Point<float> centre { 0.0f, 0.0f };
    float scale = 1.0f;
    float azimuth = 0.6f;
    float elevation = 0.9f;
};

//==============================================================================
class FemView3DComponent : public juce::Component
{
public:
    FemView3DComponent()
    {
        setOpaque (true);
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    }

    /** The mesh to draw. Pass nullptr to clear. */
    void setMesh (std::shared_ptr<const FemMesh> m)
    {
        mesh_ = std::move (m);
        fitDirty = true;
        repaint();
    }

    const std::shared_ptr<const FemMesh>& mesh() const noexcept { return mesh_; }

    /** Per-vertex heights (one per mesh vertex), or {} for a flat plate. */
    void setField (std::vector<float> vertexValues)
    {
        field = std::move (vertexValues);
        repaint();
    }

    /** Amplitude that reaches full relief. Zero (the default) re-scales every
        frame on its own maximum, which is right for a still picture and wrong
        for an animation: a decaying ring would never appear to decay. Hold a
        reference across frames to see it die away. */
    void setFieldScale (float maxAbsValue) noexcept
    {
        fieldScale = maxAbsValue;
        repaint();
    }

    /** Height at full deflection, as a fraction of the fitted plate width. */
    void setHeightScale (float fractionOfWidth) noexcept
    {
        heightScale = juce::jmax (0.0f, fractionOfWidth);
        repaint();
    }

    /** Same four colours as the flat view, so one call themes both.

        The undeflected surface is drawn in a neutral derived from the
        background rather than in the background itself. Interpolating a
        diverging map from the background would make a flat or quiet plate
        invisible — the surface would be the same colour as the space behind
        it, and only the silhouette would show. Override with
        setSurfaceColour() if the derived tone does not suit. */
    void setColours (juce::Colour background, juce::Colour lines,
                     juce::Colour negative, juce::Colour positive)
    {
        bg = background;
        lineColour = lines;
        negColour = negative;
        posColour = positive;
        surfaceColour = background.brighter (0.32f);
        repaint();
    }

    /** The colour of the undeflected surface, i.e. the middle of the
        diverging map. Call after setColours(), which derives it. */
    void setSurfaceColour (juce::Colour c)
    {
        surfaceColour = c;
        repaint();
    }

    /** Draw the mesh over the shaded facets (default), or leave the surface
        smooth. The wireframe is what turns a shaded blob into a readable
        surface plot: it gives the eye a reference grid to see the relief
        bend, which flat shading alone does not. Its cost is one stroke per
        triangle side on top of the fill, so a very fine mesh is a reason to
        turn it off rather than to accept a solid mass of lines. */
    void setShowGrid (bool shouldShow)
    {
        showGrid = shouldShow;
        repaint();
    }

    /** Drawn on top of the surface, with the projection already set up, so a
        caller can add markers in the same scene. */
    std::function<void (juce::Graphics&, FemView3DComponent&)> paintOverlay;

    /** The camera, for projecting anything else into the same scene. */
    const HeightFieldProjection& projection() const noexcept { return camera; }

    void setOrientation (float azimuthRadians, float elevationRadians)
    {
        camera.setOrientation (azimuthRadians, elevationRadians);
        repaint();
    }

    /** Magnification about the centre of the view, 1 being the fit that shows
        the whole plate however it is turned. Clamped to a range wide enough
        to inspect one element and to pull back from a tall relief, and no
        wider: past that the scene is either a single facet or a speck. */
    void setZoom (float factor)
    {
        zoom = juce::jlimit (minZoom, maxZoom, factor);
        repaint();
    }

    float getZoom() const noexcept { return zoom; }

    //==========================================================================
    void resized() override { fitDirty = true; }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragStart = e.position;
        dragAzimuth = camera.getAzimuth();
        dragElevation = camera.getElevation();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        // A drag across the full width turns the plate once round, and a drag
        // up the full height covers the whole elevation range; the view then
        // responds the same way whatever size the component happens to be.
        const auto d = e.position - dragStart;
        const float w = juce::jmax (1.0f, (float) getWidth());
        const float h = juce::jmax (1.0f, (float) getHeight());
        camera.setOrientation (dragAzimuth + juce::MathConstants<float>::twoPi * d.x / w,
                               dragElevation + juce::MathConstants<float>::halfPi * d.y / h);
        repaint();
    }

    void mouseWheelMove (const juce::MouseEvent&,
                         const juce::MouseWheelDetails& wheel) override
    {
        // isReversed is honoured explicitly so that a "natural scrolling"
        // trackpad zooms the same way a wheel does: the gesture means
        // "closer", not "wheel turned in some direction".
        const float delta = wheel.isReversed ? -wheel.deltaY : wheel.deltaY;
        if (std::abs (delta) < 1.0e-6f)
            return;

        // Multiplicative, so each notch is the same *ratio* whatever the
        // current magnification — an additive step would crawl when zoomed
        // out and leap when zoomed in.
        setZoom (zoom * std::exp (delta * 0.75f));
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (bg);
        if (mesh_ == nullptr || mesh_->empty())
            return;

        updateFit();
        // The fit is cached and the zoom is not, so the viewport is set on
        // every paint from the two together: a wheel notch then costs a
        // repaint rather than a refit.
        camera.setViewport (viewCentre, baseScale * zoom);
        projectVertices();

        const int numTris = mesh_->numTriangles();
        order.resize ((size_t) numTris);
        std::iota (order.begin(), order.end(), 0);

        triDepth.resize ((size_t) numTris);
        for (int t = 0; t < numTris; ++t)
        {
            const auto& tri = mesh_->triangles[(size_t) t];
            triDepth[(size_t) t] = (depth[(size_t) tri[0]] + depth[(size_t) tri[1]]
                                    + depth[(size_t) tri[2]]) / 3.0f;
        }
        // Far first: a nearer facet painted afterwards covers it.
        std::sort (order.begin(), order.end(), [this] (int a, int b)
        {
            return triDepth[(size_t) a] > triDepth[(size_t) b];
        });

        juce::Path facet;
        for (const int t : order)
        {
            const auto& tri = mesh_->triangles[(size_t) t];
            const auto& a = screen[(size_t) tri[0]];
            const auto& b = screen[(size_t) tri[1]];
            const auto& c = screen[(size_t) tri[2]];

            facet.clear();
            facet.startNewSubPath (a);
            facet.lineTo (b);
            facet.lineTo (c);
            facet.closeSubPath();

            g.setColour (facetColour (tri));
            g.fillPath (facet);

            // The mesh is stroked here, per facet and inside the depth-sorted
            // loop, rather than in a pass of its own afterwards. That is what
            // hides the lines on the far side of the surface: a nearer facet
            // painted later covers the edges of the ones behind it. A single
            // pass over all edges at the end would draw the whole wireframe
            // through the plate, which reads as a transparent object.
            //
            // Shared edges are therefore stroked twice, once by each adjacent
            // triangle. That is not waste but the mechanism: the stroke that
            // survives is the one belonging to the nearer facet.
            if (! showGrid)
                continue;

            const auto& sides = mesh_->triEdges[(size_t) t];
            for (int side = 0; side < 3; ++side)
            {
                const bool onBoundary = mesh_->isBoundaryEdge (sides[(size_t) side]);
                const auto& p = screen[(size_t) tri[(size_t) side]];
                const auto& q = screen[(size_t) tri[(size_t) ((side + 1) % 3)]];
                g.setColour (onBoundary ? lineColour : lineColour.withAlpha (0.28f));
                g.drawLine (p.x, p.y, q.x, q.y, onBoundary ? 1.4f : 0.7f);
            }
        }

        if (paintOverlay)
            paintOverlay (g, *this);
    }

private:
    /** Height of a vertex in scene units, normalised through the reference. */
    float heightAt (int v) const noexcept
    {
        if ((size_t) v >= field.size() || reference <= 0.0f)
            return 0.0f;
        return juce::jlimit (-1.5f, 1.5f, field[(size_t) v] / reference) * heightScale * fitSpan;
    }

    juce::Colour facetColour (const std::array<int, 3>& tri) const
    {
        // Colour from the field at the centroid: a diverging map from the
        // negative colour through the background to the positive one, so a
        // flat plate reads as the background rather than as a colour.
        float mean = 0.0f;
        if (reference > 0.0f)
            for (const int v : tri)
                if ((size_t) v < field.size())
                    mean += field[(size_t) v] / reference;
        mean = juce::jlimit (-1.0f, 1.0f, mean / 3.0f);

        const auto base = mean >= 0.0f ? surfaceColour.interpolatedWith (posColour, mean)
                                       : surfaceColour.interpolatedWith (negColour, -mean);

        // Lambert term from the facet normal, in scene space. Without it a
        // bump and a dip look identical from above, colour being symmetric
        // about the plate's own plane only in sign.
        const auto& v0 = scene[(size_t) tri[0]];
        const auto& v1 = scene[(size_t) tri[1]];
        const auto& v2 = scene[(size_t) tri[2]];
        const float ux = v1[0] - v0[0], uy = v1[1] - v0[1], uz = v1[2] - v0[2];
        const float vx = v2[0] - v0[0], vy = v2[1] - v0[1], vz = v2[2] - v0[2];
        float nx = uy * vz - uz * vy;
        float ny = uz * vx - ux * vz;
        float nz = ux * vy - uy * vx;
        const float len = std::sqrt (nx * nx + ny * ny + nz * nz);
        if (len <= 0.0f)
            return base;
        nx /= len; ny /= len; nz /= len;

        // Light over the viewer's shoulder, from above and to the left.
        constexpr float lx = -0.40f, ly = -0.35f, lz = 0.85f;
        const float lambert = std::abs (nx * lx + ny * ly + nz * lz);
        return base.withMultipliedBrightness (0.55f + 0.75f * lambert);
    }

    void updateFit()
    {
        // The amplitude reference: the caller's, or this frame's own maximum.
        reference = fieldScale;
        if (reference <= 0.0f)
        {
            for (const float v : field)
                reference = juce::jmax (reference, std::abs (v));
            if (reference <= 0.0f)
                reference = 1.0f;
        }

        if (! fitDirty)
            return;
        fitDirty = false;

        double minx = 1e30, miny = 1e30, maxx = -1e30, maxy = -1e30;
        for (const auto& p : mesh_->vertices)
        {
            minx = std::min (minx, p.x); maxx = std::max (maxx, p.x);
            miny = std::min (miny, p.y); maxy = std::max (maxy, p.y);
        }
        cx = 0.5 * (minx + maxx);
        cy = 0.5 * (miny + maxy);
        fitSpan = (float) std::max (1.0e-12, std::max (maxx - minx, maxy - miny));

        // Room for the relief and for the plate turning edge-on, so that
        // neither the tallest peak nor the widest rotation leaves the view.
        const float margin = 1.5f;
        const float px = juce::jmax (1.0f, (float) getWidth()) / (margin * fitSpan);
        const float py = juce::jmax (1.0f, (float) getHeight()) / (margin * fitSpan);
        baseScale = std::min (px, py);
        viewCentre = { 0.5f * (float) getWidth(), 0.5f * (float) getHeight() };
    }

    void projectVertices()
    {
        const int n = mesh_->numVertices();
        screen.resize ((size_t) n);
        depth.resize ((size_t) n);
        scene.resize ((size_t) n);

        for (int v = 0; v < n; ++v)
        {
            const auto& p = mesh_->vertices[(size_t) v];
            const float x = (float) (p.x - cx);
            const float y = (float) (p.y - cy);
            const float z = heightAt (v);
            scene[(size_t) v] = { x, y, z };
            screen[(size_t) v] = camera.project (x, y, z, &depth[(size_t) v]);
        }
    }

    std::shared_ptr<const FemMesh> mesh_;
    std::vector<float> field;

    HeightFieldProjection camera;
    std::vector<juce::Point<float>> screen;
    std::vector<float> depth, triDepth;
    std::vector<std::array<float, 3>> scene;
    std::vector<int> order;

    juce::Colour bg { 0xff141a24 }, lineColour { 0xff4a5670 };
    juce::Colour surfaceColour { juce::Colour (0xff141a24).brighter (0.32f) };
    juce::Colour negColour { 0xff4cc9f0 }, posColour { 0xffe0784a };

    static constexpr float minZoom = 0.3f, maxZoom = 12.0f;

    bool showGrid = true;
    float zoom = 1.0f;
    float baseScale = 1.0f;
    juce::Point<float> viewCentre;

    float fieldScale = 0.0f, reference = 1.0f;
    float heightScale = 0.22f;
    float fitSpan = 1.0f;
    double cx = 0.0, cy = 0.0;
    bool fitDirty = true;

    juce::Point<float> dragStart;
    float dragAzimuth = 0.0f, dragElevation = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FemView3DComponent)
};

} // namespace fxme::acoustics
