/*
  ------------------------------------------------------------------------------
    FemViewComponent.h

    JUCE component displaying a FemMesh (FemMesh.h) and, optionally, a nodal
    scalar field on it as filled contours (banded diverging colour map),
    e.g. a plate eigenmode from PlateModes.h.

    Message-thread only, like any juce::Component. Typical use:

        fxme::acoustics::FemViewComponent view;
        view.setMesh (meshPtr);                  // shared_ptr<const FemMesh>
        view.setField (result.shapes[k]);        // per-vertex values (or {})
        view.onPlateClick = [] (double x, double y, const juce::MouseEvent&) {
            ...   // plate coordinates (same units as the mesh)
        };
        view.paintOverlay = [] (juce::Graphics& g, fxme::acoustics::FemViewComponent& v) {
            auto p = v.plateToScreen (0.5, 0.5);  // draw markers on top
            ...
        };

    The mesh is fitted into the component bounds preserving aspect ratio,
    with the plate y axis pointing up. The field image is cached and only
    re-rasterised when the mesh, field or size changes.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "FemMesh.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

namespace fxme::acoustics
{

class FemViewComponent : public juce::Component
{
public:
    FemViewComponent() { setOpaque (false); }

    /** Sets (or clears, with nullptr) the mesh to display. The mesh is
        shared, not copied: do not mutate it afterwards. */
    void setMesh (std::shared_ptr<const FemMesh> m)
    {
        mesh_ = std::move (m);
        updateFit();
        fieldDirty = true;
        repaint();
    }

    std::shared_ptr<const FemMesh> mesh() const { return mesh_; }

    /** Per-vertex scalar field to draw as filled contours; pass an empty
        vector to show the bare grid. */
    void setField (std::vector<float> vertexValues)
    {
        field = std::move (vertexValues);
        fieldDirty = true;
        repaint();
    }

    void setShowGrid (bool shouldShow)     { showGrid = shouldShow; repaint(); }
    void setContourLevels (int numLevels)  { levels = juce::jmax (2, numLevels); fieldDirty = true; repaint(); }

    void setColours (juce::Colour background, juce::Colour grid,
                     juce::Colour negativeAccent, juce::Colour positiveAccent)
    {
        bgColour = background;
        gridColour = grid;
        negColour = negativeAccent;
        posColour = positiveAccent;
        fieldDirty = true;
        repaint();
    }

    /** Plate -> component coordinates (plate y up). Valid once a mesh is set
        and the component has a size. */
    juce::Point<float> plateToScreen (double x, double y) const
    {
        return { (float) (offX + scale * x), (float) (offY - scale * y) };
    }

    juce::Point<double> screenToPlate (juce::Point<float> p) const
    {
        if (scale <= 0.0)
            return { 0.0, 0.0 };
        return { ((double) p.x - offX) / scale, (offY - (double) p.y) / scale };
    }

    /** Called on mouse-down with plate coordinates (only when a mesh is set). */
    std::function<void (double, double, const juce::MouseEvent&)> onPlateClick;
    /** Same, for drags following such a mouse-down. */
    std::function<void (double, double, const juce::MouseEvent&)> onPlateDrag;

    /** Drawn last, over grid and contours: markers, boundary decorations... */
    std::function<void (juce::Graphics&, FemViewComponent&)> paintOverlay;

    //==========================================================================
    void resized() override
    {
        updateFit();
        fieldDirty = true;
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (mesh_ != nullptr && onPlateClick)
        {
            const auto p = screenToPlate (e.position);
            onPlateClick (p.x, p.y, e);
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (mesh_ != nullptr && onPlateDrag)
        {
            const auto p = screenToPlate (e.position);
            onPlateDrag (p.x, p.y, e);
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (bgColour);
        if (mesh_ == nullptr || mesh_->empty())
        {
            g.setColour (gridColour);
            g.setFont (14.0f);
            g.drawText ("no mesh", getLocalBounds(), juce::Justification::centred);
            return;
        }

        if (! field.empty() && field.size() == (size_t) mesh_->numVertices())
        {
            if (fieldDirty)
                renderFieldImage();
            if (fieldImage.isValid())
                g.drawImageAt (fieldImage, 0, 0);
        }

        if (showGrid || field.empty())
        {
            const auto& m = *mesh_;
            g.setColour (gridColour.withAlpha (field.empty() ? 0.8f : 0.35f));
            for (int e = 0; e < m.numEdges(); ++e)
            {
                const auto& ed = m.edges[(size_t) e];
                const auto a = plateToScreen (m.vertices[(size_t) ed.v0].x, m.vertices[(size_t) ed.v0].y);
                const auto b = plateToScreen (m.vertices[(size_t) ed.v1].x, m.vertices[(size_t) ed.v1].y);
                g.drawLine (a.x, a.y, b.x, b.y, m.isBoundaryEdge (e) ? 1.6f : 0.6f);
            }
        }

        if (paintOverlay)
            paintOverlay (g, *this);
    }

private:
    void updateFit()
    {
        scale = 0.0;
        if (mesh_ == nullptr || mesh_->empty() || getWidth() <= 0 || getHeight() <= 0)
            return;

        double minx = 1e30, miny = 1e30, maxx = -1e30, maxy = -1e30;
        for (const auto& p : mesh_->vertices)
        {
            minx = std::min (minx, p.x); maxx = std::max (maxx, p.x);
            miny = std::min (miny, p.y); maxy = std::max (maxy, p.y);
        }
        const double w = std::max (1.0e-12, maxx - minx);
        const double h = std::max (1.0e-12, maxy - miny);

        const double margin = 8.0;
        scale = std::min (((double) getWidth() - 2 * margin) / w,
                          ((double) getHeight() - 2 * margin) / h);
        offX = 0.5 * ((double) getWidth() - scale * (minx + maxx));
        offY = 0.5 * ((double) getHeight() + scale * (miny + maxy));
    }

    juce::Colour colourForValue (float t) const
    {
        // t in [-1, 1], quantised into `levels` bands, diverging colour map:
        // negativeAccent <- dark centre -> positiveAccent.
        const float q = std::round (t * (float) (levels - 1)) / (float) (levels - 1);
        const float a = std::pow (std::abs (q), 0.7f);   // perceptual-ish ramp
        const auto centre = bgColour.brighter (0.15f);
        return q >= 0.0f ? centre.interpolatedWith (posColour, a)
                         : centre.interpolatedWith (negColour, a);
    }

    void renderFieldImage()
    {
        fieldDirty = false;
        const int w = getWidth(), h = getHeight();
        if (w <= 0 || h <= 0 || mesh_ == nullptr || scale <= 0.0)
        {
            fieldImage = {};
            return;
        }

        float maxAbs = 1.0e-30f;
        for (float v : field)
            maxAbs = std::max (maxAbs, std::abs (v));

        fieldImage = juce::Image (juce::Image::ARGB, w, h, true);
        juce::Image::BitmapData bits (fieldImage, juce::Image::BitmapData::writeOnly);

        const auto& m = *mesh_;
        for (int ti = 0; ti < m.numTriangles(); ++ti)
        {
            const auto& t = m.triangles[(size_t) ti];
            const auto A = plateToScreen (m.vertices[(size_t) t[0]].x, m.vertices[(size_t) t[0]].y);
            const auto B = plateToScreen (m.vertices[(size_t) t[1]].x, m.vertices[(size_t) t[1]].y);
            const auto C = plateToScreen (m.vertices[(size_t) t[2]].x, m.vertices[(size_t) t[2]].y);
            const float v0 = field[(size_t) t[0]] / maxAbs;
            const float v1 = field[(size_t) t[1]] / maxAbs;
            const float v2 = field[(size_t) t[2]] / maxAbs;

            const float det = (B.x - A.x) * (C.y - A.y) - (C.x - A.x) * (B.y - A.y);
            if (std::abs (det) < 1.0e-12f)
                continue;

            const int x0 = juce::jlimit (0, w - 1, (int) std::floor (std::min ({ A.x, B.x, C.x })));
            const int x1 = juce::jlimit (0, w - 1, (int) std::ceil  (std::max ({ A.x, B.x, C.x })));
            const int y0 = juce::jlimit (0, h - 1, (int) std::floor (std::min ({ A.y, B.y, C.y })));
            const int y1 = juce::jlimit (0, h - 1, (int) std::ceil  (std::max ({ A.y, B.y, C.y })));

            for (int py = y0; py <= y1; ++py)
            {
                for (int px = x0; px <= x1; ++px)
                {
                    const float fx = (float) px + 0.5f, fy = (float) py + 0.5f;
                    const float l1 = ((fx - A.x) * (C.y - A.y) - (C.x - A.x) * (fy - A.y)) / det;
                    const float l2 = ((B.x - A.x) * (fy - A.y) - (fx - A.x) * (B.y - A.y)) / det;
                    const float l0 = 1.0f - l1 - l2;
                    // Small negative tolerance: leaves no seams between triangles.
                    if (l0 < -0.02f || l1 < -0.02f || l2 < -0.02f)
                        continue;
                    const float v = juce::jlimit (-1.0f, 1.0f, l0 * v0 + l1 * v1 + l2 * v2);
                    bits.setPixelColour (px, py, colourForValue (v));
                }
            }
        }
    }

    std::shared_ptr<const FemMesh> mesh_;
    std::vector<float> field;
    juce::Image fieldImage;
    bool fieldDirty = true;
    bool showGrid = true;
    int levels = 13;

    double scale = 0.0, offX = 0.0, offY = 0.0;

    juce::Colour bgColour   { 0xff17141f };
    juce::Colour gridColour { 0xff58506a };
    juce::Colour negColour  { 0xff4cc9f0 };
    juce::Colour posColour  { 0xffe0784a };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FemViewComponent)
};

} // namespace fxme::acoustics
