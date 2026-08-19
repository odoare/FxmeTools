/*
  ==============================================================================

    SequencerRubber.h

    JUCE component that renders a StringSequencer as a horizontal rubber band:
    - Mouse: left-drag on empty space creates a block; left-click selects
      (clicking the selected block again deselects); dragging a block's body
      moves it and dragging its left/right edge resizes it (both with walls
      against the neighbours — no overlap is allowed); alt-click deletes a
      block. The resize zone scales with the step width and is capped at a
      third of the block, so short blocks stay draggable.
    - Copying: cmd/ctrl-drag a block duplicates it (content included) where
      it is dropped, and cmd/ctrl-shift-drag copies only its content onto the
      block it is dropped on. Both leave the source alone and commit on
      mouse-up, so an invalid drop simply does nothing.
    - Any of those three — plus a plain body drag, which becomes a *move* —
      is handed to the owner once the cursor leaves this rubber
      (onCopyDragMoved / onCopyDropped, see CopyDrag): a drag never leaves
      the component it started in, so only the parent can see a sibling.
      While outside, a move stops mutating the source, so a refused drop
      leaves the block exactly where it was.
    - Keyboard: Delete removes the selected block; cmd/ctrl-D duplicates it
      into the steps immediately after it and selects the copy, so repeating
      the key lays down a run. Alt-right-click clears a block's content —
      alt being the "destroy" modifier throughout (alt-click deletes it).
    - Mouse wheel: horizontal scroll.
    - A moving playhead line and per-block custom painting via a BlockPainter.

    Blocks that the StringSequencer reports as dormant (start step past the
    current window, e.g. after a grid shrink) are not drawn and cannot be
    grabbed; an amber arrow at the pattern's right edge shows they are there.

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
    /** A block's content was set from another block (duplicate, text copy):
        the owner should re-parse it. Separate from onBlockContentCleared so
        existing users keep their meaning of "cleared". */
    std::function<void (int blockId)> onBlockContentChanged;

    // ---- copy drags that leave this rubber ---------------------------------

    /** A copy drag (cmd-drag, cmd-shift-drag) reported to the owner.

        A drag keeps the mouse captured by the component it started in, so a
        rubber can never see a drop on a sibling — only the parent that lays
        them out can. These two callbacks hand it the drag: everything about
        *where* a cross-component drop may land, and the ghost drawn over the
        target, belongs to the owner. Leaving them null keeps the rubber
        entirely self-contained. */
    struct CopyDrag
    {
        enum class Kind
        {
            Move,       // plain body drag that left the rubber
            Duplicate,  // cmd-drag: the block and its content
            Content     // cmd-shift-drag: the override string only
        };

        Kind kind          = Kind::Move;
        bool insideSource  = true;   // cursor still over the rubber it started in
        int  sourceBlockId = -1;
        int  lengthSteps   = 1;      // Move/Duplicate: the source's length
        int  grabOffset    = 0;      // grabbed step relative to the block's start, so
                                     // the owner can resolve the landing step against
                                     // the TARGET's geometry (scroll may differ)
        juce::Point<int> screenPos;  // to find the component under the cursor
    };

    /** Every drag move during a copy/move drag: the owner updates (or clears)
        its own drop ghost. Also called while the cursor is still inside, with
        insideSource true, so the owner knows to drop its ghost. */
    std::function<void (const CopyDrag&)> onCopyDragMoved;

    /** Mouse-up of such a drag that ended OUTSIDE this rubber. The owner
        decides whether it is legal and commits it; the rubber does nothing —
        in particular a Move leaves the source block alone, so a refused drop
        loses nothing. */
    std::function<void (const CopyDrag&)> onCopyDropped;

    // ---- geometry, for an owner drawing over this rubber -------------------

    /** The step under `x` (this component's coordinates). */
    int stepAtX (int x) const noexcept { return xToStep (x); }

    /** The block under `p` (this component's coordinates), or -1. */
    int blockIdAt (juce::Point<int> p) const { return hitTest (p).blockId; }

    /** The rectangle a step range occupies, in this component's coordinates. */
    juce::Rectangle<int> rectForSteps (int fromStep, int toStep) const noexcept
    {
        const int x0 = stepToX (fromStep);
        const int x1 = stepToX (toStep);
        return { x0, 0, std::max (0, x1 - x0), getHeight() };
    }

    /** The rectangle a block occupies; empty if unknown or dormant. */
    juce::Rectangle<int> rectForBlock (int blockId) const noexcept
    {
        if (const auto* b = seq_.blockById (blockId))
            if (seq_.isInRange (*b))
                return blockRect (*b);
        return {};
    }

    // ---- paint + interaction -----------------------------------------------

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds();
        g.setColour (juce::Colour (0xff181e28));
        g.fillRect (bounds);

        // Step dividers
        const int ns = seq_.getNumSteps();
        g.setColour (juce::Colour (0xff2a3040));
        for (int s = 0; s <= ns; ++s)
        {
            const int x = stepToX (s);
            g.drawVerticalLine (x, 0.0f, (float) getHeight());
        }

        // Blocks
        for (const auto& b : seq_.blocks())
        {
            if (! seq_.isInRange (b)) continue;   // dormant: past the window

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

        // Dormant blocks live past the window: mark the pattern's end so a
        // grid shrink reads as "there is more beyond here", not as data loss.
        if (seq_.dormantCount() > 0)
        {
            const float h  = (float) getHeight();
            const float th = juce::jlimit (5.0f, 11.0f, h * 0.45f);
            const float x1 = (float) stepToX (ns) - 2.0f;
            const float cy = h * 0.5f;
            juce::Path tri;
            tri.addTriangle (x1 - th * 0.6f, cy - th * 0.5f,
                             x1 - th * 0.6f, cy + th * 0.5f,
                             x1,             cy);
            g.setColour (juce::Colour (0xffe8a33c));
            g.fillPath (tri);
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
            // Alt-right-click: clear the clicked block's content. A plain
            // right-click was far too easy to land by accident for something
            // that throws text away, and alt is already the modifier that
            // means "destroy this" here (alt-click deletes the block).
            if (e.mods.isAltDown())
            {
                const auto hit = hitTest (e.getPosition());
                if (hit.blockId >= 0)
                {
                    seq_.clearContent (hit.blockId);
                    if (onBlockContentCleared) onBlockContentCleared (hit.blockId);
                    repaint();
                }
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

        // Cmd/Ctrl-drag on a block duplicates it; add Shift to copy only its
        // content onto another block. Both leave the source untouched and
        // commit on mouse-up, so nothing changes until the drop is valid.
        if (hit.blockId >= 0 && e.mods.isCommandDown())
        {
            dragMode_    = e.mods.isShiftDown() ? Drag::CopyingContent : Drag::Duplicating;
            dragBlockId_ = hit.blockId;
            dropTarget_  = -1;
            copyInside_  = true;
            if (const auto* b = seq_.blockById (hit.blockId))
            {
                dragGrabOffset_ = xToStep (e.x) - b->startStep;
                ghostStart_     = b->startStep;
                ghostLen_       = b->endStep - b->startStep;
            }
            repaint();
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
                copyInside_     = true;
                if (const auto* b = seq_.blockById (hit.blockId))
                {
                    dragGrabOffset_ = xToStep (e.x) - b->startStep;
                    ghostLen_       = b->endStep - b->startStep;
                }
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
        else if (dragMode_ == Drag::Moving
                 || dragMode_ == Drag::Duplicating || dragMode_ == Drag::CopyingContent)
        {
            const bool inside = getLocalBounds().contains (e.getPosition());
            if (inside != copyInside_) { copyInside_ = inside; repaint(); }

            if (! inside)
            {
                // The owner takes over: drop this rubber's own ghost so the
                // two never show at once. A Move stops mutating the source
                // here too — it is left exactly where it was until the owner
                // commits, so a refused drop costs nothing.
                if (dropTarget_ != -1) { dropTarget_ = -1; repaint(); }
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
            else if (dragMode_ == Drag::Duplicating)
            {
                const int newStart = step - dragGrabOffset_;
                if (newStart != ghostStart_) { ghostStart_ = newStart; repaint(); }
            }
            else
            {
                // Only another block is a valid drop; dropping on empty space
                // (or back on the source) does nothing.
                const auto hit = hitTest (e.getPosition());
                const int  t   = (hit.blockId != dragBlockId_) ? hit.blockId : -1;
                if (t != dropTarget_) { dropTarget_ = t; repaint(); }
            }

            if (onCopyDragMoved)
                onCopyDragMoved (makeCopyDrag (e));
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        // A body click that never became a move toggles the selection off.
        // Not when it left the lane: that is a cross-lane move, and the drop
        // decides the selection.
        if (dragMode_ == Drag::Moving && copyInside_ && ! dragMoved_ && clickedSelected_)
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
        else if (! copyInside_
                 && (dragMode_ == Drag::Moving || dragMode_ == Drag::Duplicating
                     || dragMode_ == Drag::CopyingContent))
        {
            // Dropped on a sibling: only the owner can judge and commit it.
            if (onCopyDropped)
                onCopyDropped (makeCopyDrag (e));
        }
        else if (dragMode_ == Drag::Duplicating)
        {
            placeCopy (dragBlockId_, ghostStart_, ghostLen_);
        }
        else if (dragMode_ == Drag::CopyingContent)
        {
            if (dropTarget_ >= 0)
            {
                // Read the source out before touching the sequencer: any
                // mutation may reorder its vector and dangle the pointer.
                std::string content;
                if (const auto* src = seq_.blockById (dragBlockId_))
                    content = src->content;

                const int target = dropTarget_;
                if (seq_.setContent (target, content) && onBlockContentChanged)
                    onBlockContentChanged (target);
            }
        }

        dragMode_    = Drag::None;
        dragBlockId_ = -1;
        dropTarget_  = -1;
        copyInside_  = true;
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
        // Cmd/Ctrl-D: drop a copy of the selected block immediately after it
        // and select that, so repeating the key lays down a run of blocks.
        // Strictly adjacent — placing the copy anywhere else would put it
        // where the user is not looking.
        if (k == juce::KeyPress ('d', juce::ModifierKeys::commandModifier, 0))
        {
            if (selectedBlockId_ >= 0)
                if (const auto* b = seq_.blockById (selectedBlockId_))
                    placeCopy (selectedBlockId_, b->endStep, b->endStep - b->startStep);
            return true;   // consumed even when there was no room: no beep
        }

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

    // ---- drag overlays (drawn on top of normal paint) ----------------------

    void paintOverChildren (juce::Graphics& g) override
    {
        if (dragMode_ == Drag::Creating)
        {
            const int s0 = std::min (dragOriginStep_, createStep_);
            const int s1 = std::max (dragOriginStep_, createStep_) + 1;
            paintGhost (g, s0, s1, true);
        }
        else if (! copyInside_)
        {
            // The cursor left: the owner is drawing the ghost over whatever
            // it is on now, and two ghosts at once would be worse than none.
        }
        else if (dragMode_ == Drag::Duplicating)
        {
            // Red while the copy would not fit: the drop is refused, and
            // saying so during the drag beats a click that does nothing. The
            // source is NOT excluded — it stays put, so its own steps are
            // occupied, and a cmd-click that never moves places nothing.
            paintGhost (g, ghostStart_, ghostStart_ + ghostLen_,
                        seq_.canPlaceBlock (ghostStart_, ghostLen_));
        }
        else if (dragMode_ == Drag::CopyingContent)
        {
            if (const auto* t = seq_.blockById (dropTarget_))
            {
                const auto r = blockRect (*t);
                g.setColour (juce::Colours::white.withAlpha (0.25f));
                g.fillRect (r);
                g.setColour (juce::Colours::white);
                g.drawRect (r, 2);
            }
        }
    }

private:
    void paintGhost (juce::Graphics& g, int fromStep, int toStep, bool valid)
    {
        const int x0 = stepToX (fromStep);
        const int x1 = stepToX (toStep);
        if (x1 <= x0) return;

        const auto c = valid ? juce::Colours::white : juce::Colour (0xffe8483c);
        g.setColour (c.withAlpha (0.18f));
        g.fillRect (x0, 0, x1 - x0, getHeight());
        g.setColour (c.withAlpha (0.55f));
        g.drawRect (x0, 0, x1 - x0, getHeight(), 1);
    }

    /** Adds a copy of `sourceId` at [startStep, startStep+len) and selects it,
        or does nothing at all if that range is not free. The source's content
        is read out first: adding a block may reorder the sequencer's vector
        and dangle any pointer into it. */
    void placeCopy (int sourceId, int startStep, int len)
    {
        const auto* src = seq_.blockById (sourceId);
        if (src == nullptr || ! seq_.canPlaceBlock (startStep, len))
            return;

        const std::string content = src->content;
        const int id = seq_.addBlock (startStep, len);
        if (id < 0)
            return;

        seq_.setContent (id, content);
        selectBlock (id);
        // Content first: the owner re-parses on this callback, and selecting
        // the copy before that would show it as an unparsed (red) block.
        if (onBlockContentChanged) onBlockContentChanged (id);
        if (onBlockSelected)       onBlockSelected (id);
        repaint();
    }

    StringSequencer& seq_;
    BlockPainter     painter_;

    double scrollPixels_  = 0.0;
    double playheadStep_  = 0.0;
    int    activeBlockId_   = -1;
    int    selectedBlockId_ = -1;

    enum class Drag { None, Creating, ResizingStart, ResizingEnd, Moving,
                      Duplicating, CopyingContent };
    Drag dragMode_        = Drag::None;
    int  dragBlockId_     = -1;
    int  dragOriginStep_  = 0;
    int  createStep_      = 0;
    int  dragGrabOffset_  = 0;      // Moving/Duplicating: grabbed step relative to block start
    bool dragMoved_       = false;  // Moving: the block actually moved
    bool clickedSelected_ = false;  // Moving: the click hit the selected block
    int  ghostStart_      = 0;      // Duplicating: where the copy would land
    int  ghostLen_        = 1;
    int  dropTarget_      = -1;     // CopyingContent: block under the cursor
    bool copyInside_      = true;   // copy drag: cursor still over this rubber

    CopyDrag makeCopyDrag (const juce::MouseEvent& e) const
    {
        CopyDrag d;
        d.kind          = dragMode_ == Drag::CopyingContent ? CopyDrag::Kind::Content
                        : dragMode_ == Drag::Duplicating    ? CopyDrag::Kind::Duplicate
                                                            : CopyDrag::Kind::Move;
        d.insideSource  = copyInside_;
        d.sourceBlockId = dragBlockId_;
        d.lengthSteps   = ghostLen_;
        d.grabOffset    = dragGrabOffset_;
        d.screenPos     = e.getScreenPosition();
        return d;
    }

    static constexpr int kEdgeGrab      = 10; // px width for edge grab zone
    static constexpr int kMaxEdgeGrab   = 16; // ...its ceiling on wide steps
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
        // playableEnd, not endStep: a block left straddling the window edge by
        // a grid shrink is drawn (and grabbed) up to the edge only.
        const int x0 = stepToX (b.startStep);
        const int x1 = stepToX (seq_.playableEnd (b));
        return juce::Rectangle<int> (x0, 0, x1 - x0, getHeight()).reduced (1, 1);
    }

    /** Width of the resize zone at each end of `r`. It grows with the step
        width (wide steps make a fixed 10 px feel needlessly fine), but never
        takes more than a third of a short block: the middle has to stay
        draggable, or a one-step block could only ever be resized, never
        moved — which is what the old fixed 7 px did at small step widths. */
    int edgeGrab (juce::Rectangle<int> r) const noexcept
    {
        const int wanted = juce::jlimit (kEdgeGrab, kMaxEdgeGrab,
                                         (int) std::round (pixelsPerStep() * 0.25));
        return juce::jmax (2, juce::jmin (wanted, r.getWidth() / 3));
    }

    struct HitResult { int blockId = -1; bool leftEdge = false; bool rightEdge = false; };

    HitResult hitTest (juce::Point<int> p) const
    {
        for (const auto& b : seq_.blocks())
        {
            if (! seq_.isInRange (b)) continue;

            const auto r = blockRect (b);
            if (! r.contains (p)) continue;

            const int  grab = edgeGrab (r);
            const bool le   = (p.x < r.getX() + grab);
            const bool re   = (p.x > r.getRight() - grab);
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
