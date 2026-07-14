/*
  ==============================================================================

    SequencerRubber.h

    JUCE component that renders a StringSequencer as a horizontal rubber band:
    - Mouse: left-drag on empty space creates a block; left-click selects
      (clicking the selected block again deselects); dragging a block's body
      moves it and dragging its left/right edge resizes it (both with walls
      against the neighbours — no overlap is allowed); alt-click deletes a
      block.
    - Keyboard: Delete removes the selected block; right-click clears its content.
    - Mouse wheel: horizontal scroll.
    - A moving playhead line and per-block custom painting via a BlockPainter.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../midi/StringSequencer.h"

namespace fxme
{

class SequencerRubber : public juce::Component
{
public:
    /** Callback that paints the interior of one block. The bounds are the
        block's rectangle in component coordinates.
        `isSelected` – the block is currently selected in the rubber.
        `isPlaying`  – the sequencer is currently sounding this block. */
    using BlockPainter = std::function<void (juce::Graphics&,
                                             juce::Rectangle<int>,
                                             const SeqBlock&,
                                             bool isSelected,
                                             bool isPlaying)>;

    SequencerRubber (StringSequencer& seq, BlockPainter painter)
        : seq_ (seq), painter_ (std::move (painter))
    {
        setWantsKeyboardFocus (true);
    }

    /** Minimum width of one step in pixels. When numSteps * min exceeds the
        component width the strip becomes wider than the component and
        scrolls horizontally (mouse wheel) — the default 20 px keeps steps
        grabbable. Set 1 to always fit the whole pattern in the component
        (e.g. when several strips must stay visually aligned). */
    void setMinPixelsPerStep (int pixels)
    {
        minPixPerStep_ = std::max (1, pixels);
        scrollPixels_ = 0.0;
        repaint();
    }

    // ---- state pushed by the outer component --------------------------------

    /** Called by a timer (message thread) to update the moving playhead. */
    void setPlayheadStep (double step)
    {
        if (step != playheadStep_) { playheadStep_ = step; repaint(); }
    }

    /** The block currently being sounded by the sequencer (shown in active
        colour inside the block). Pass -1 when nothing is active. */
    void setActiveBlockId (int id)
    {
        if (id != activeBlockId_) { activeBlockId_ = id; repaint(); }
    }

    // ---- selection ----------------------------------------------------------

    int  selectedBlockId() const noexcept { return selectedBlockId_; }

    void selectBlock (int id)
    {
        if (id != selectedBlockId_) { selectedBlockId_ = id; repaint(); }
    }

    // ---- callbacks wired by the outer component ----------------------------
    std::function<void (int blockId)> onBlockSelected;   // -1 = deselected
    std::function<void (int blockId)> onBlockDeleted;
    std::function<void (int blockId)> onBlockContentCleared;

    // ---- paint + interaction -----------------------------------------------

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds();
        g.setColour (juce::Colour (0xff181e28));
        g.fillRect (bounds);

        // Step dividers
        const double pps = pixelsPerStep();
        const int    ns  = seq_.getNumSteps();
        g.setColour (juce::Colour (0xff2a3040));
        for (int s = 0; s <= ns; ++s)
        {
            const int x = stepToX (s);
            g.drawVerticalLine (x, 0.0f, (float) getHeight());
        }

        // Blocks
        for (const auto& b : seq_.blocks())
        {
            const auto r = blockRect (b);
            if (r.getRight() < 0 || r.getX() > getWidth()) continue;

            const bool selected = (b.id == selectedBlockId_);
            const bool playing  = (b.id == activeBlockId_);

            if (painter_)
                painter_ (g, r, b, selected, playing);
            else
                paintDefaultBlock (g, r, b, selected, playing);

            // Selection border
            if (selected)
            {
                g.setColour (juce::Colours::white.withAlpha (0.7f));
                g.drawRect (r, 2);
            }
        }

        // Playhead
        const int phx = stepToX (playheadStep_);
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.drawVerticalLine (phx, 0.0f, (float) getHeight());
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();

        if (e.mods.isRightButtonDown())
        {
            // Right-click: clear content of the clicked block
            const auto hit = hitTest (e.getPosition());
            if (hit.blockId >= 0)
            {
                seq_.clearContent (hit.blockId);
                if (onBlockContentCleared) onBlockContentCleared (hit.blockId);
                repaint();
            }
            return;
        }

        const auto hit = hitTest (e.getPosition());

        // Alt-click: delete the block (a mouse-only alternative to the
        // Delete key, which needs the keyboard focus a hosted plugin window
        // does not always win).
        if (e.mods.isAltDown())
        {
            if (hit.blockId >= 0)
            {
                const bool wasSelected = (hit.blockId == selectedBlockId_);
                if (wasSelected)
                    selectedBlockId_ = -1;
                seq_.removeBlock (hit.blockId);
                if (onBlockDeleted)                onBlockDeleted (hit.blockId);
                if (wasSelected && onBlockSelected) onBlockSelected (-1);
                repaint();
            }
            dragMode_    = Drag::None;
            dragBlockId_ = -1;
            return;
        }

        if (hit.blockId >= 0)
        {
            if (hit.leftEdge || hit.rightEdge)
            {
                // Begin edge resize
                dragMode_    = hit.leftEdge ? Drag::ResizingStart : Drag::ResizingEnd;
                dragBlockId_ = hit.blockId;
                const auto* b = seq_.blockById (hit.blockId);
                dragOriginStep_ = hit.leftEdge ? b->startStep : b->endStep;
            }
            else
            {
                // Body click: select (if not already) and arm a whole-block
                // move. A click on the already-selected block that never
                // turns into a drag deselects on mouse-up.
                clickedSelected_ = (hit.blockId == selectedBlockId_);
                if (! clickedSelected_)
                {
                    selectBlock (hit.blockId);
                    if (onBlockSelected) onBlockSelected (hit.blockId);
                }
                dragMode_       = Drag::Moving;
                dragBlockId_    = hit.blockId;
                dragMoved_      = false;
                if (const auto* b = seq_.blockById (hit.blockId))
                    dragGrabOffset_ = xToStep (e.x) - b->startStep;
            }
        }
        else
        {
            // Click on empty space: begin block creation
            const int step = xToStep (e.x);
            if (step >= 0 && step < seq_.getNumSteps())
            {
                dragMode_       = Drag::Creating;
                dragBlockId_    = -1;
                dragOriginStep_ = step;
                createStep_     = step;
            }
            // Deselect
            if (selectedBlockId_ >= 0)
            {
                selectedBlockId_ = -1;
                if (onBlockSelected) onBlockSelected (-1);
                repaint();
            }
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.mods.isRightButtonDown()) return;

        const int step = xToStep (e.x);

        if (dragMode_ == Drag::Creating)
        {
            // Rubber-band preview: track end of drag but don't commit yet.
            // We preview by repainting with a ghost rectangle.
            createStep_ = step;
            repaint();
        }
        else if (dragMode_ == Drag::ResizingStart)
        {
            seq_.moveBlockStart (dragBlockId_, step);
            repaint();
        }
        else if (dragMode_ == Drag::ResizingEnd)
        {
            seq_.moveBlockEnd (dragBlockId_, step);
            repaint();
        }
        else if (dragMode_ == Drag::Moving)
        {
            if (const auto* b = seq_.blockById (dragBlockId_))
            {
                const int newStart = step - dragGrabOffset_;
                if (newStart != b->startStep)
                {
                    seq_.moveBlock (dragBlockId_, newStart);
                    dragMoved_ = true;
                    repaint();
                }
            }
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        // A body click that never became a move toggles the selection off.
        if (dragMode_ == Drag::Moving && ! dragMoved_ && clickedSelected_)
        {
            selectedBlockId_ = -1;
            if (onBlockSelected) onBlockSelected (-1);
        }

        if (dragMode_ == Drag::Creating)
        {
            const int s0 = std::min (dragOriginStep_, createStep_);
            const int s1 = std::max (dragOriginStep_, createStep_);
            const int dur = std::max (1, s1 - s0 + 1);

            const int id = seq_.addBlock (s0, dur);
            if (id >= 0)
            {
                selectBlock (id);
                if (onBlockSelected) onBlockSelected (id);
            }
        }
        dragMode_    = Drag::None;
        dragBlockId_ = -1;
        repaint();
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto hit = hitTest (e.getPosition());
        setMouseCursor ((hit.leftEdge || hit.rightEdge)
                        ? juce::MouseCursor::LeftRightResizeCursor
                        : juce::MouseCursor::NormalCursor);
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        setMouseCursor (juce::MouseCursor::NormalCursor);
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        const double maxScroll = std::max (0.0, contentWidth() - getWidth());
        scrollPixels_ = std::max (0.0, std::min (maxScroll, scrollPixels_ - w.deltaX * 60.0));
        repaint();
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey)
        {
            if (selectedBlockId_ >= 0)
            {
                const int id = selectedBlockId_;
                selectedBlockId_ = -1;
                seq_.removeBlock (id);
                if (onBlockDeleted)    onBlockDeleted (id);
                if (onBlockSelected)   onBlockSelected (-1);
                repaint();
                return true;
            }
        }
        return false;
    }

    // ---- creation ghost overlay (drawn on top of normal paint) -------------

    void paintOverChildren (juce::Graphics& g) override
    {
        if (dragMode_ != Drag::Creating) return;

        const int s0 = std::min (dragOriginStep_, createStep_);
        const int s1 = std::max (dragOriginStep_, createStep_) + 1;
        const int x0 = stepToX (s0);
        const int x1 = stepToX (s1);
        if (x1 <= x0) return;

        g.setColour (juce::Colours::white.withAlpha (0.18f));
        g.fillRect (x0, 0, x1 - x0, getHeight());
        g.setColour (juce::Colours::white.withAlpha (0.55f));
        g.drawRect (x0, 0, x1 - x0, getHeight(), 1);
    }

private:
    StringSequencer& seq_;
    BlockPainter     painter_;

    double scrollPixels_  = 0.0;
    double playheadStep_  = 0.0;
    int    activeBlockId_   = -1;
    int    selectedBlockId_ = -1;

    enum class Drag { None, Creating, ResizingStart, ResizingEnd, Moving };
    Drag dragMode_        = Drag::None;
    int  dragBlockId_     = -1;
    int  dragOriginStep_  = 0;
    int  createStep_      = 0;
    int  dragGrabOffset_  = 0;      // Moving: grabbed step relative to block start
    bool dragMoved_       = false;  // Moving: the block actually moved
    bool clickedSelected_ = false;  // Moving: the click hit the selected block

    static constexpr int kEdgeGrab      = 7;  // px width for edge grab zone
    static constexpr int kMinPixPerStep = 20; // default minimum pixels per step

    int minPixPerStep_ = kMinPixPerStep;

    double pixelsPerStep() const noexcept
    {
        const int ns = seq_.getNumSteps();
        if (ns <= 0) return 1.0;
        return std::max ((double) minPixPerStep_,
                         (double) getWidth() / (double) ns);
    }

    double contentWidth() const noexcept
    {
        return pixelsPerStep() * seq_.getNumSteps();
    }

    int stepToX (double step) const noexcept
    {
        return (int) std::round (step * pixelsPerStep() - scrollPixels_);
    }

    int xToStep (int x) const noexcept
    {
        const double pps = pixelsPerStep();
        return (pps > 0.0)
            ? (int) std::floor (((double) x + scrollPixels_) / pps)
            : 0;
    }

    juce::Rectangle<int> blockRect (const SeqBlock& b) const noexcept
    {
        const int x0 = stepToX (b.startStep);
        const int x1 = stepToX (b.endStep);
        return juce::Rectangle<int> (x0, 0, x1 - x0, getHeight()).reduced (1, 1);
    }

    struct HitResult { int blockId = -1; bool leftEdge = false; bool rightEdge = false; };

    HitResult hitTest (juce::Point<int> p) const
    {
        for (const auto& b : seq_.blocks())
        {
            const auto r = blockRect (b);
            if (! r.contains (p)) continue;

            const bool le = (p.x < r.getX() + kEdgeGrab);
            const bool re = (p.x > r.getRight() - kEdgeGrab);
            return { b.id, le, re };
        }
        return {};
    }

    void paintDefaultBlock (juce::Graphics& g, juce::Rectangle<int> r,
                            const SeqBlock&, bool selected, bool playing) const
    {
        const juce::Colour bg = playing  ? juce::Colour (0xff8800aa)
                              : selected ? juce::Colour (0xff224466)
                                         : juce::Colour (0xff1a3050);
        g.setColour (bg);
        g.fillRect (r);
    }
};

} // namespace fxme
