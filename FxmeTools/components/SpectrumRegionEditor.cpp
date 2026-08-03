/*
  ------------------------------------------------------------------------------
    SpectrumRegionEditor.cpp

    See SpectrumRegionEditor.h.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include "SpectrumRegionEditor.h"

namespace fxme
{

SpectrumRegionEditor::SpectrumRegionEditor()
{
    setWantsKeyboardFocus (true);
}

//==============================================================================
void SpectrumRegionEditor::setNumRegions (int numRegions)
{
    regions.assign ((size_t) juce::jmax (0, numRegions), {});
    selected = -1;
    repaint();
}

void SpectrumRegionEditor::setRegion (int index, const Region& region)
{
    if (! juce::isPositiveAndBelow (index, (int) regions.size()))
        return;

    regions[(size_t) index] = region;
    if (! region.active && selected == index)
        selected = -1;

    repaint();
}

SpectrumRegionEditor::Region SpectrumRegionEditor::getRegion (int index) const
{
    return juce::isPositiveAndBelow (index, (int) regions.size()) ? regions[(size_t) index]
                                                                  : Region {};
}

void SpectrumRegionEditor::setGainRange (float newMinDb, float newMaxDb)
{
    gainMinDb = juce::jmin (newMinDb, newMaxDb);
    gainMaxDb = juce::jmax (newMinDb, newMaxDb);
    repaint();
}

void SpectrumRegionEditor::setGateRange (float newMinDb, float newMaxDb)
{
    gateMinDb = juce::jmin (newMinDb, newMaxDb);
    gateMaxDb = juce::jmax (newMinDb, newMaxDb);
    repaint();
}

void SpectrumRegionEditor::setMinimumRegionRatio (float ratio)
{
    minRatio = juce::jmax (1.0001f, ratio);
}

void SpectrumRegionEditor::setSelectedRegion (int index)
{
    const int wanted = juce::isPositiveAndBelow (index, (int) regions.size()) ? index : -1;
    if (wanted == selected)
        return;

    selected = wanted;
    if (onSelectionChanged != nullptr)
        onSelectionChanged (selected);
    repaint();
}

juce::Rectangle<int> SpectrumRegionEditor::getRegionArea (int index) const
{
    if (! juce::isPositiveAndBelow (index, (int) regions.size()))
        return {};

    const auto& r = regions[(size_t) index];
    if (! r.active)
        return {};

    return regionBounds (r, getPlotArea()).toNearestInt();
}

//==============================================================================
juce::Rectangle<float> SpectrumRegionEditor::regionBounds (const Region& r,
                                                           juce::Rectangle<float> plot) const
{
    const float x0 = freqToX (juce::jmin (r.lowHz, r.highHz), plot);
    const float x1 = freqToX (juce::jmax (r.lowHz, r.highHz), plot);
    return { x0, plot.getY(), juce::jmax (1.0f, x1 - x0), plot.getHeight() };
}

float SpectrumRegionEditor::gainToY (float db, juce::Rectangle<float> plot) const
{
    return juce::jmap (juce::jlimit (gainMinDb, gainMaxDb, db),
                       gainMinDb, gainMaxDb, plot.getBottom(), plot.getY());
}

float SpectrumRegionEditor::yToGain (float y, juce::Rectangle<float> plot) const
{
    return juce::jmap (juce::jlimit (plot.getY(), plot.getBottom(), y),
                       plot.getBottom(), plot.getY(), gainMinDb, gainMaxDb);
}

float SpectrumRegionEditor::xToPan (float x, float left, float right)
{
    const float w = juce::jmax (1.0f, right - left);
    return juce::jlimit (-1.0f, 1.0f, 2.0f * (x - left) / w - 1.0f);
}

juce::Point<float> SpectrumRegionEditor::gainPanHandle (const Region& r,
                                                        juce::Rectangle<float> plot) const
{
    const auto b = regionBounds (r, plot);
    const float pan = juce::jlimit (-1.0f, 1.0f, r.pan);
    return { b.getX() + (pan + 1.0f) * 0.5f * b.getWidth(), gainToY (r.gainDb, plot) };
}

void SpectrumRegionEditor::enforceMinimumWidth (Region& r, bool movingLow) const
{
    if (r.highHz >= r.lowHz * minRatio)
        return;

    // Push the border that is not under the pointer, so the one being dragged
    // keeps following it.
    if (movingLow)
        r.lowHz = r.highHz / minRatio;
    else
        r.highHz = r.lowHz * minRatio;
}

//==============================================================================
SpectrumRegionEditor::Hit SpectrumRegionEditor::hitTestRegion (juce::Point<float> p) const
{
    const auto plot = getPlotArea();
    if (! plot.contains (p))
        return {};

    // Walk from the selected region outwards, then back to front, so the region
    // the user is working on keeps priority where rectangles overlap.
    std::vector<int> order;
    order.reserve (regions.size());
    if (juce::isPositiveAndBelow (selected, (int) regions.size()))
        order.push_back (selected);
    for (int i = (int) regions.size() - 1; i >= 0; --i)
        if (i != selected)
            order.push_back (i);

    // Handles first, across every region: a border or a grab point must stay
    // reachable even when another region's body covers it.
    for (auto pass : { 0, 1 })
    {
        for (int i : order)
        {
            const auto& r = regions[(size_t) i];
            if (! r.active)
                continue;

            const auto b = regionBounds (r, plot);

            if (pass == 0)
            {
                if (p.getDistanceFrom (gainPanHandle (r, plot)) <= handleRadius + 3.0f)
                    return { i, Handle::gainPan };

                if (std::abs (p.x - b.getX()) <= edgeGrabPx)
                    return { i, Handle::leftEdge };

                if (std::abs (p.x - b.getRight()) <= edgeGrabPx)
                    return { i, Handle::rightEdge };

                if (p.x >= b.getX() && p.x <= b.getRight())
                {
                    if (std::abs (p.y - dbToY (r.gateDb, plot)) <= lineGrabPx)
                        return { i, Handle::gate };
                    if (std::abs (p.y - gainToY (r.gainDb, plot)) <= lineGrabPx)
                        return { i, Handle::gainPan };
                }
            }
            else if (b.contains (p))
            {
                return { i, Handle::body };
            }
        }
    }

    return {};
}

bool SpectrumRegionEditor::isBaseGesture (const juce::MouseEvent& e) const
{
    return overBadge (e.getPosition()) || overLegend (e.getPosition())
        || e.mods.isMiddleButtonDown() || e.mods.isAltDown();
}

//==============================================================================
void SpectrumRegionEditor::mouseDown (const juce::MouseEvent& e)
{
    dragHandle = Handle::none;
    dragIndex  = -1;
    dragMoved  = false;
    creating   = false;

    baseGesture = isBaseGesture (e);
    if (baseGesture)
    {
        SpectrumDisplay::mouseDown (e);
        return;
    }

    grabKeyboardFocus();

    const auto hit = hitTestRegion (e.position);
    if (hit.index >= 0)
    {
        setSelectedRegion (hit.index);
        dragIndex  = hit.index;
        dragHandle = hit.handle;
        dragStartRegion = regions[(size_t) hit.index];
        dragStartPos = e.position;

        if (onDragStart != nullptr)
            onDragStart (dragIndex, dragHandle);
        return;
    }

    // Empty space: start drawing a new region.
    const auto plot = getPlotArea();
    if (! plot.contains (e.position))
        return;

    setSelectedRegion (-1);
    creating = true;
    createFromHz = createToHz = xToFreq (e.position.x, plot);
    repaint();
}

void SpectrumRegionEditor::mouseDrag (const juce::MouseEvent& e)
{
    const auto plot = getPlotArea();

    if (creating)
    {
        createToHz = xToFreq (e.position.x, plot);
        dragMoved = true;
        repaint();
        return;
    }

    if (dragIndex < 0)
    {
        if (baseGesture)
            SpectrumDisplay::mouseDrag (e);
        return;
    }

    dragMoved = dragMoved || e.getDistanceFromDragStart() > 2;

    Region r = dragStartRegion;

    switch (dragHandle)
    {
        case Handle::leftEdge:
            r.lowHz = xToFreq (e.position.x, plot);
            enforceMinimumWidth (r, true);
            break;

        case Handle::rightEdge:
            r.highHz = xToFreq (e.position.x, plot);
            enforceMinimumWidth (r, false);
            break;

        case Handle::gate:
            r.gateDb = juce::jlimit (gateMinDb, gateMaxDb, yToDb (e.position.y, plot));
            break;

        case Handle::gainPan:
        {
            r.gainDb = yToGain (e.position.y, plot);
            const auto b = regionBounds (r, plot);
            r.pan = xToPan (e.position.x, b.getX(), b.getRight());
            break;
        }

        case Handle::body:
        {
            // Move the whole band: a horizontal drag is a frequency ratio, so
            // the region keeps its width on the log axis.
            const float ratio = xToFreq (e.position.x, plot)
                                    / juce::jmax (1.0e-3f, xToFreq (dragStartPos.x, plot));
            r.lowHz  = dragStartRegion.lowHz  * ratio;
            r.highHz = dragStartRegion.highHz * ratio;
            break;
        }

        case Handle::none:
        default:
            return;
    }

    // Everything stays inside the analyser's range; a band pushed against an
    // end keeps its width rather than collapsing onto the edge.
    const float span = juce::jmax (1.0f, r.highHz / juce::jmax (1.0e-3f, r.lowHz));
    if (r.lowHz < SpectrumAnalyzer::fMin)
    {
        r.lowHz = SpectrumAnalyzer::fMin;
        if (dragHandle == Handle::body)
            r.highHz = r.lowHz * span;
    }
    if (r.highHz > SpectrumAnalyzer::fMax)
    {
        r.highHz = SpectrumAnalyzer::fMax;
        if (dragHandle == Handle::body)
            r.lowHz = r.highHz / span;
    }
    r.lowHz  = juce::jlimit (SpectrumAnalyzer::fMin, SpectrumAnalyzer::fMax, r.lowHz);
    r.highHz = juce::jlimit (SpectrumAnalyzer::fMin, SpectrumAnalyzer::fMax, r.highHz);

    regions[(size_t) dragIndex] = r;
    if (onRegionChanged != nullptr)
        onRegionChanged (dragIndex, r);

    repaint();
}

void SpectrumRegionEditor::mouseUp (const juce::MouseEvent& e)
{
    if (creating)
    {
        const float lo = juce::jmin (createFromHz, createToHz);
        const float hi = juce::jmax (createFromHz, createToHz);
        creating = false;

        // Too narrow to be meant: treat it as a click on the background.
        if (dragMoved && hi >= lo * minRatio && onRegionCreate != nullptr)
        {
            const int index = onRegionCreate (lo, hi);
            if (index >= 0)
                setSelectedRegion (index);
        }

        repaint();
        return;
    }

    baseGesture = false;

    if (dragIndex >= 0)
    {
        if (onDragEnd != nullptr)
            onDragEnd (dragIndex, dragHandle);

        // A press and release on the body with nothing in between is a click:
        // the consumer opens that region's settings.
        if (! dragMoved && dragHandle == Handle::body && onRegionClicked != nullptr)
            onRegionClicked (dragIndex);

        dragIndex = -1;
        dragHandle = Handle::none;
        return;
    }

    juce::ignoreUnused (e);
}

void SpectrumRegionEditor::mouseMove (const juce::MouseEvent& e)
{
    SpectrumDisplay::mouseMove (e);

    const auto hit = hitTestRegion (e.position);
    if (hit.index != hover.index || hit.handle != hover.handle)
    {
        hover = hit;
        repaint();
    }

    switch (hit.handle)
    {
        case Handle::leftEdge:
        case Handle::rightEdge: setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); break;
        case Handle::gate:      setMouseCursor (juce::MouseCursor::UpDownResizeCursor);    break;
        case Handle::gainPan:   setMouseCursor (juce::MouseCursor::PointingHandCursor);    break;
        case Handle::body:      setMouseCursor (juce::MouseCursor::DraggingHandCursor);    break;
        case Handle::none:
        default:                setMouseCursor (juce::MouseCursor::NormalCursor);          break;
    }
}

bool SpectrumRegionEditor::keyPressed (const juce::KeyPress& key)
{
    const bool isDelete = key == juce::KeyPress::deleteKey
                       || key == juce::KeyPress::backspaceKey;

    if (isDelete && selected >= 0 && onRegionDelete != nullptr)
    {
        const int index = selected;
        setSelectedRegion (-1);
        onRegionDelete (index);
        return true;
    }

    return SpectrumDisplay::keyPressed (key);
}

//==============================================================================
juce::String SpectrumRegionEditor::dbText (float db)
{
    return (db > 0.0f ? "+" : "") + juce::String (db, 1) + " dB";
}

void SpectrumRegionEditor::drawRegion (juce::Graphics& g, const Region& r,
                                       juce::Rectangle<float> plot, bool isSelected,
                                       bool fillOnly) const
{
    const auto b = regionBounds (r, plot);

    if (fillOnly)
    {
        g.setColour (r.colour.withAlpha (isSelected ? 0.20f : 0.10f));
        g.fillRect (b);
        return;
    }

    // Borders: the pair of handles that set the band's edges.
    g.setColour (r.colour.withAlpha (isSelected ? 1.0f : 0.7f));
    const float borderWidth = isSelected ? 2.0f : 1.2f;
    g.fillRect (b.getX() - borderWidth * 0.5f, b.getY(), borderWidth, b.getHeight());
    g.fillRect (b.getRight() - borderWidth * 0.5f, b.getY(), borderWidth, b.getHeight());

    // The gate, on the plot's own dB axis so it can be read against the trace.
    // Drawn dashed, and only where it is inside the visible dB window.
    const float gateY = dbToY (r.gateDb, plot);
    if (gateY >= plot.getY() && gateY <= plot.getBottom())
    {
        const float dashes[] { 4.0f, 3.0f };
        g.setColour (r.colour.withAlpha (isSelected ? 0.95f : 0.55f));
        g.drawDashedLine ({ b.getX(), gateY, b.getRight(), gateY }, dashes, 2, 1.4f);
    }

    // Gain and pan: one segment across the band with the round handle on it.
    const float gainY = gainToY (r.gainDb, plot);
    g.setColour (r.colour.withAlpha (isSelected ? 0.9f : 0.5f));
    g.drawLine (b.getX(), gainY, b.getRight(), gainY, isSelected ? 2.0f : 1.4f);

    const auto handle = gainPanHandle (r, plot);
    g.setColour (r.colour.brighter (isSelected ? 0.4f : 0.0f));
    g.fillEllipse (juce::Rectangle<float> (handleRadius * 2.0f, handleRadius * 2.0f)
                       .withCentre (handle));
    g.setColour (getColours().plotBackground);
    g.drawEllipse (juce::Rectangle<float> (handleRadius * 2.0f, handleRadius * 2.0f)
                       .withCentre (handle), 1.2f);

    // Label in the corner, and the values of whatever is being worked on.
    g.setFont (10.0f);
    if (r.label.isNotEmpty() && b.getWidth() > 16.0f)
    {
        g.setColour (r.colour.withAlpha (isSelected ? 1.0f : 0.7f));
        g.drawText (r.label, b.withHeight (13.0f).reduced (3.0f, 0.0f),
                    juce::Justification::centredLeft);
    }

    if (isSelected && b.getWidth() > 60.0f)
    {
        g.setColour (getColours().text.withAlpha (0.85f));
        if (gateY >= plot.getY() && gateY <= plot.getBottom())
            g.drawText ("gate " + dbText (r.gateDb),
                        juce::Rectangle<float> (b.getX() + 4.0f, gateY - 13.0f,
                                                b.getWidth() - 8.0f, 12.0f),
                        juce::Justification::centredRight);

        g.drawText (dbText (r.gainDb),
                    juce::Rectangle<float> (b.getX() + 4.0f, gainY - 13.0f,
                                            b.getWidth() - 8.0f, 12.0f),
                    juce::Justification::centredLeft);
    }
}

void SpectrumRegionEditor::paintBehindTraces (juce::Graphics& g, juce::Rectangle<float> plot)
{
    for (int i = 0; i < (int) regions.size(); ++i)
        if (regions[(size_t) i].active)
            drawRegion (g, regions[(size_t) i], plot, i == selected, true);
}

void SpectrumRegionEditor::paintOverTraces (juce::Graphics& g, juce::Rectangle<float> plot)
{
    for (int i = 0; i < (int) regions.size(); ++i)
        if (regions[(size_t) i].active && i != selected)
            drawRegion (g, regions[(size_t) i], plot, false, false);

    // The selected region goes last so its handles stay on top.
    if (juce::isPositiveAndBelow (selected, (int) regions.size())
        && regions[(size_t) selected].active)
        drawRegion (g, regions[(size_t) selected], plot, true, false);

    if (creating)
    {
        const float x0 = freqToX (juce::jmin (createFromHz, createToHz), plot);
        const float x1 = freqToX (juce::jmax (createFromHz, createToHz), plot);
        juce::Rectangle<float> b (x0, plot.getY(), juce::jmax (1.0f, x1 - x0), plot.getHeight());

        g.setColour (getColours().text.withAlpha (0.15f));
        g.fillRect (b);
        g.setColour (getColours().text.withAlpha (0.7f));
        g.drawRect (b, 1.0f);
    }
}

} // namespace fxme
