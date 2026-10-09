/*
  ------------------------------------------------------------------------------
    BreakpointCurveEditor.h

    Draws and edits an fxme::CurveShape (core synth/BreakpointCurve.h): the
    breakpoints of a modulation curve, the curvature of each segment, and its
    sustain region.

    Gestures:
      drag a point                 move it (the first and last stay at the
                                   edges; x stays between its neighbours)
      shift + drag                 snap to a 1/16 grid
      double-click empty space     add a point there
      double-click a point         remove it (not the first or last)
      drag a segment's mid handle  bend the segment (up: slow start)
      double-click a mid handle    straighten the segment
      drag a sustain marker        move the region's start / end (snaps to
                                   points); shown while the sustain is on

    The shape is the caller's: setShape() shows one, onChange receives every
    edit (call it the single source of truth and write it back wherever the
    shape lives, a ValueTree say). setPlayheads() draws markers at live
    positions (normalised time). Colours via setColours().

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>
#include <FxmeTools/synth/BreakpointCurve.h>

namespace fxme
{

class BreakpointCurveEditor : public juce::Component,
                              public juce::SettableTooltipClient
{
public:
    struct Colours
    {
        juce::Colour background { 0xff121216 };
        juce::Colour grid       { 0x22ffffff };
        juce::Colour curve      { 0xffff8a1f };
        juce::Colour fill       { 0x30ff8a1f };
        juce::Colour point      { 0xffe8e8ee };
        juce::Colour sustain    { 0x3040c0ff };
        juce::Colour playhead   { 0xccffffff };
    };

    std::function<void (const CurveShape&)> onChange;
    std::function<void()> onGestureStart, onGestureEnd;

    BreakpointCurveEditor() { setRepaintsOnMouseActivity (true); }

    void setColours (Colours c) { colours = c; repaint(); }

    void setShape (const CurveShape& s)
    {
        if (dragging != Drag::none)
            return;   // do not fight the gesture in progress
        shape = s;
        shape.sanitise();
        repaint();
    }

    const CurveShape& getShape() const noexcept { return shape; }

    /** Live playhead positions in normalised time (empty: none). */
    void setPlayheads (const std::vector<float>& positions)
    {
        if (positions != playheads)
        {
            playheads = positions;
            repaint();
        }
    }

    //==========================================================================
    void paint (juce::Graphics& g) override
    {
        const auto r = plotArea();
        g.setColour (colours.background);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);

        g.setColour (colours.grid);
        for (int i = 1; i < 8; ++i)
        {
            const float x = r.getX() + r.getWidth() * (float) i / 8.0f;
            g.drawVerticalLine ((int) x, r.getY(), r.getBottom());
        }
        for (int i = 1; i < 4; ++i)
        {
            const float y = r.getY() + r.getHeight() * (float) i / 4.0f;
            g.drawHorizontalLine ((int) y, r.getX(), r.getRight());
        }

        if (shape.sustainEnabled)
        {
            const float x0 = toX (shape.x[(size_t) shape.sustainStart]);
            const float x1 = toX (shape.x[(size_t) shape.sustainEnd]);
            g.setColour (colours.sustain);
            g.fillRect (juce::Rectangle<float> (x0, r.getY(), juce::jmax (2.0f, x1 - x0), r.getHeight()));
            g.setColour (colours.sustain.withAlpha (0.9f));
            g.drawVerticalLine ((int) x0, r.getY(), r.getBottom());
            g.drawVerticalLine ((int) x1, r.getY(), r.getBottom());
            g.fillRect (juce::Rectangle<float> (x0 - 4.0f, r.getY(), 8.0f, 6.0f));
            g.fillRect (juce::Rectangle<float> (x1 - 4.0f, r.getY(), 8.0f, 6.0f));
        }

        // The curve, sampled per pixel.
        juce::Path p;
        const int w = juce::jmax (2, (int) r.getWidth());
        for (int i = 0; i <= w; ++i)
        {
            const float t = (float) i / (float) w;
            const auto pt = juce::Point<float> (r.getX() + (float) i, toY (shape.valueAt (t)));
            if (i == 0) p.startNewSubPath (pt); else p.lineTo (pt);
        }
        juce::Path fill (p);
        fill.lineTo (r.getRight(), r.getBottom());
        fill.lineTo (r.getX(), r.getBottom());
        fill.closeSubPath();
        g.setColour (colours.fill);
        g.fillPath (fill);
        g.setColour (isEnabled() ? colours.curve : colours.curve.withMultipliedSaturation (0.3f));
        g.strokePath (p, juce::PathStrokeType (1.8f));

        // Segment handles, then points.
        for (int i = 0; i < shape.numPoints - 1; ++i)
        {
            const auto h = handlePosition (i);
            g.setColour (colours.curve.withAlpha (hoverHandle == i ? 1.0f : 0.5f));
            g.drawEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (h), 1.2f);
        }
        for (int i = 0; i < shape.numPoints; ++i)
        {
            const auto pt = pointPosition (i);
            g.setColour (colours.point.withAlpha (hoverPoint == i ? 1.0f : 0.8f));
            g.fillEllipse (juce::Rectangle<float> (hoverPoint == i ? 9.0f : 7.0f, hoverPoint == i ? 9.0f : 7.0f).withCentre (pt));
        }

        g.setColour (colours.playhead);
        for (auto t : playheads)
        {
            const float x = toX (juce::jlimit (0.0f, 1.0f, t));
            g.drawVerticalLine ((int) x, r.getY(), r.getBottom());
            g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ x, toY (shape.valueAt (t)) }));
        }
    }

    //==========================================================================
    void mouseMove (const juce::MouseEvent& e) override
    {
        const int p = hitPoint (e.position), h = p < 0 ? hitHandle (e.position) : -1;
        if (p != hoverPoint || h != hoverHandle)
        {
            hoverPoint = p;
            hoverHandle = h;
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hoverPoint = hoverHandle = -1;
        repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = Drag::none;
        if (const int s = hitSustainMarker (e.position); s >= 0)
        {
            dragging = s == 0 ? Drag::sustainStart : Drag::sustainEnd;
        }
        else if ((dragIndex = hitPoint (e.position)) >= 0)
        {
            dragging = Drag::point;
        }
        else if ((dragIndex = hitHandle (e.position)) >= 0)
        {
            dragging = Drag::curvature;
            dragStartCurve = shape.curve[(size_t) dragIndex];
        }

        if (dragging != Drag::none && onGestureStart)
            onGestureStart();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const auto r = plotArea();
        float t = juce::jlimit (0.0f, 1.0f, (e.position.x - r.getX()) / r.getWidth());
        float v = juce::jlimit (0.0f, 1.0f, (r.getBottom() - e.position.y) / r.getHeight());
        if (e.mods.isShiftDown())
        {
            t = std::round (t * 16.0f) / 16.0f;
            v = std::round (v * 16.0f) / 16.0f;
        }

        switch (dragging)
        {
            case Drag::point:
            {
                const auto i = (size_t) dragIndex;
                if (dragIndex > 0 && dragIndex < shape.numPoints - 1)
                    shape.x[i] = juce::jlimit (shape.x[i - 1], shape.x[i + 1], t);
                shape.y[i] = v;
                break;
            }
            case Drag::curvature:
            {
                // The mid handle follows the mouse: up raises the middle of
                // the segment, whichever way the segment goes.
                const float dy = (float) -e.getDistanceFromDragStartY() / juce::jmax (20.0f, r.getHeight() * 0.5f);
                const auto i = (size_t) dragIndex;
                const float dir = shape.y[i + 1] >= shape.y[i] ? 1.0f : -1.0f;
                shape.curve[i] = juce::jlimit (-1.0f, 1.0f, dragStartCurve - dy * dir);
                break;
            }
            case Drag::sustainStart:
                shape.sustainStart = juce::jmin (nearestPoint (t), shape.sustainEnd);
                break;
            case Drag::sustainEnd:
                shape.sustainEnd = juce::jmax (nearestPoint (t), shape.sustainStart);
                break;
            case Drag::none:
                return;
        }

        shape.sanitise();
        changed();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging != Drag::none && onGestureEnd)
            onGestureEnd();
        dragging = Drag::none;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (const int p = hitPoint (e.position); p >= 0)
        {
            if (p > 0 && p < shape.numPoints - 1)
                removePoint (p);
        }
        else if (const int h = hitHandle (e.position); h >= 0)
        {
            shape.curve[(size_t) h] = 0.0f;
        }
        else
        {
            const auto r = plotArea();
            addPoint (juce::jlimit (0.0f, 1.0f, (e.position.x - r.getX()) / r.getWidth()),
                      juce::jlimit (0.0f, 1.0f, (r.getBottom() - e.position.y) / r.getHeight()));
        }
        shape.sanitise();
        changed();
    }

private:
    enum class Drag { none, point, curvature, sustainStart, sustainEnd };

    juce::Rectangle<float> plotArea() const { return getLocalBounds().toFloat().reduced (8.0f, 10.0f); }
    float toX (float t) const { const auto r = plotArea(); return r.getX() + t * r.getWidth(); }
    float toY (float v) const { const auto r = plotArea(); return r.getBottom() - v * r.getHeight(); }

    juce::Point<float> pointPosition (int i) const { return { toX (shape.x[(size_t) i]), toY (shape.y[(size_t) i]) }; }

    juce::Point<float> handlePosition (int i) const
    {
        const float t = 0.5f * (shape.x[(size_t) i] + shape.x[(size_t) i + 1]);
        return { toX (t), toY (shape.valueAt (t)) };
    }

    int hitPoint (juce::Point<float> p) const
    {
        for (int i = 0; i < shape.numPoints; ++i)
            if (pointPosition (i).getDistanceFrom (p) < 7.0f)
                return i;
        return -1;
    }

    int hitHandle (juce::Point<float> p) const
    {
        for (int i = 0; i < shape.numPoints - 1; ++i)
            if (handlePosition (i).getDistanceFrom (p) < 6.0f)
                return i;
        return -1;
    }

    int hitSustainMarker (juce::Point<float> p) const
    {
        if (! shape.sustainEnabled || p.y > plotArea().getY() + 10.0f)
            return -1;
        if (std::abs (p.x - toX (shape.x[(size_t) shape.sustainStart])) < 6.0f) return 0;
        if (std::abs (p.x - toX (shape.x[(size_t) shape.sustainEnd])) < 6.0f)   return 1;
        return -1;
    }

    int nearestPoint (float t) const
    {
        int best = 0;
        for (int i = 1; i < shape.numPoints; ++i)
            if (std::abs (shape.x[(size_t) i] - t) < std::abs (shape.x[(size_t) best] - t))
                best = i;
        return best;
    }

    void addPoint (float t, float v)
    {
        if (shape.numPoints >= CurveShape::maxPoints)
            return;
        int at = 1;
        while (at < shape.numPoints && shape.x[(size_t) at] < t)
            ++at;
        for (int i = shape.numPoints; i > at; --i)
        {
            shape.x[(size_t) i] = shape.x[(size_t) i - 1];
            shape.y[(size_t) i] = shape.y[(size_t) i - 1];
            shape.curve[(size_t) i] = shape.curve[(size_t) i - 1];
        }
        shape.x[(size_t) at] = t;
        shape.y[(size_t) at] = v;
        shape.curve[(size_t) at] = 0.0f;
        ++shape.numPoints;
        if (shape.sustainStart >= at) ++shape.sustainStart;
        if (shape.sustainEnd >= at)   ++shape.sustainEnd;
    }

    void removePoint (int p)
    {
        for (int i = p; i < shape.numPoints - 1; ++i)
        {
            shape.x[(size_t) i] = shape.x[(size_t) i + 1];
            shape.y[(size_t) i] = shape.y[(size_t) i + 1];
            shape.curve[(size_t) i] = shape.curve[(size_t) i + 1];
        }
        --shape.numPoints;
        if (shape.sustainStart > p) --shape.sustainStart;
        if (shape.sustainEnd >= p && shape.sustainEnd > 0) --shape.sustainEnd;
        hoverPoint = -1;
    }

    void changed()
    {
        repaint();
        if (onChange)
            onChange (shape);
    }

    CurveShape shape;
    Colours colours;
    std::vector<float> playheads;
    Drag dragging = Drag::none;
    int dragIndex = -1, hoverPoint = -1, hoverHandle = -1;
    float dragStartCurve = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BreakpointCurveEditor)
};

} // namespace fxme
