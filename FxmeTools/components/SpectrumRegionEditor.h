/*
  ------------------------------------------------------------------------------
    SpectrumRegionEditor.h

    A spectrum view you can draw bands on. Built on fxme::SpectrumDisplay, so
    it keeps the log-frequency grid, the live traces, the zoom and the badges,
    and adds a fixed pool of rectangular regions over the top.

    A region is a frequency interval carrying three values, each with its own
    grab handle inside the rectangle:

      - the borders, dragged left and right;
      - a gate, a horizontal segment on the dB axis of the plot itself, so it
        can be read straight against the trace it crosses (what rises above the
        segment is what the consumer's gate is meant to pass);
      - a gain and a pan, one horizontal segment with a round handle on it:
        dragging the handle up and down sets the gain (on its own range, see
        setGainRange) and left to right sets the pan across the region's width.

    Empty space is where new regions are drawn: press and drag sideways, and
    onRegionCreate is asked for a free slot. Pressing a region selects it, a
    press without a drag reports a click (open its settings), and Delete or
    Backspace removes the selected one. Panning the view, which would otherwise
    collide with drawing, moves to alt-drag or the middle button.

    The component holds no authority over the values: every gesture reports
    through a callback, and the consumer pushes the result back with
    setRegion(). That keeps a parameter-backed host (an APVTS, say) as the
    single source of truth, and makes automation and preset loads show up here
    with no extra wiring.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include "SpectrumDisplay.h"
#include <functional>
#include <vector>

namespace fxme
{

class SpectrumRegionEditor : public SpectrumDisplay
{
public:
    struct Region
    {
        bool  active = false;          // an inactive slot is not drawn
        float lowHz  = 20.0f;
        float highHz = 20000.0f;
        float gateDb = -100.0f;
        float gainDb = 0.0f;
        float pan    = 0.0f;           // -1 = left edge of the region, +1 = right
        juce::Colour colour { 0xff35d6d0 };
        juce::String label;            // drawn in the rectangle's top-left corner
    };

    /** Which part of a region a press landed on. Reported to the consumer only
        through the drag callbacks; useful to know when reading them. */
    enum class Handle { none, leftEdge, rightEdge, gate, gainPan, body };

    SpectrumRegionEditor();

    //==========================================================================
    /** Size of the region pool. Slots keep their index for the component's
        whole life, so a consumer can map slot to parameter set one to one. */
    void setNumRegions (int numRegions);
    int  getNumRegions() const noexcept          { return (int) regions.size(); }

    /** Pushes one slot's values in (parameters to view). Fires no callback. */
    void setRegion (int index, const Region& region);
    Region getRegion (int index) const;

    /** Range the gain handle's vertical travel maps onto (default -60 to +12
        dB). Unlike the gate, the gain is not a level on the plot's own dB
        axis, so it needs its own. */
    void setGainRange (float newMinDb, float newMaxDb);

    /** Range the gate segment is clamped to while dragging. Defaults to the
        plot's own dB range, which is usually what is wanted. */
    void setGateRange (float newMinDb, float newMaxDb);

    /** Narrowest region that can be drawn or dragged out, as the ratio between
        its borders (default 1.06, roughly a semitone). */
    void setMinimumRegionRatio (float ratio);

    int  getSelectedRegion() const noexcept      { return selected; }
    void setSelectedRegion (int index);

    //==========================================================================
    // Callbacks, all delivered on the message thread.

    /** A drag moved one of region `index`'s values. Sent continuously while
        dragging; the region carries the new values. */
    std::function<void (int index, const Region& region)> onRegionChanged;

    /** A region was drawn on empty space. Return the slot that took it, or -1
        to refuse (nothing free). The consumer is expected to set that slot's
        values and push them back with setRegion(). */
    std::function<int (float lowHz, float highHz)> onRegionCreate;

    /** Delete or Backspace was pressed with region `index` selected. */
    std::function<void (int index)> onRegionDelete;

    /** A press and release inside region `index` with no drag in between: the
        consumer usually opens that region's settings. */
    std::function<void (int index)> onRegionClicked;

    std::function<void (int index)> onSelectionChanged;

    /** Bracket a drag, so a parameter-backed consumer can wrap it in a host
        automation gesture. `index` is the region being dragged. */
    std::function<void (int index, Handle handle)> onDragStart, onDragEnd;

    //==========================================================================
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

protected:
    void paintBehindTraces (juce::Graphics&, juce::Rectangle<float> plot) override;
    void paintOverTraces (juce::Graphics&, juce::Rectangle<float> plot) override;

private:
    struct Hit
    {
        int index = -1;
        Handle handle = Handle::none;
    };

    Hit hitTestRegion (juce::Point<float> p) const;

    /** True when this press belongs to the base class (a badge, a legend entry,
        or an explicit pan gesture) rather than to region editing. */
    bool isBaseGesture (const juce::MouseEvent& e) const;

    juce::Rectangle<float> regionBounds (const Region& r, juce::Rectangle<float> plot) const;
    juce::Point<float> gainPanHandle (const Region& r, juce::Rectangle<float> plot) const;

    float gainToY (float db, juce::Rectangle<float> plot) const;
    float yToGain (float y, juce::Rectangle<float> plot) const;
    static float xToPan (float x, float left, float right);

    /** Keeps a region's borders apart by at least the minimum ratio, moving
        whichever border was not the one being dragged. */
    void enforceMinimumWidth (Region& r, bool movingLow) const;

    void drawRegion (juce::Graphics&, const Region&, juce::Rectangle<float> plot,
                     bool isSelected, bool fillOnly) const;

    static juce::String dbText (float db);

    std::vector<Region> regions;
    int selected = -1;

    // Live gesture state. baseGesture records that the press was handed to
    // SpectrumDisplay, so only those drags are forwarded on: forwarding one the
    // base class never saw a mouseDown for would pan from stale anchors.
    bool   baseGesture = false;
    Handle dragHandle = Handle::none;
    int    dragIndex  = -1;
    Region dragStartRegion;
    juce::Point<float> dragStartPos;
    bool   dragMoved = false;

    // Drawing a new region on empty space.
    bool  creating = false;
    float createFromHz = 0.0f, createToHz = 0.0f;

    Hit hover;

    float gainMinDb = -60.0f, gainMaxDb = 12.0f;
    float gateMinDb = -100.0f, gateMaxDb = 0.0f;
    float minRatio = 1.06f;

    static constexpr float edgeGrabPx   = 5.0f;
    static constexpr float lineGrabPx   = 5.0f;
    static constexpr float handleRadius = 5.5f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumRegionEditor)
};

} // namespace fxme
