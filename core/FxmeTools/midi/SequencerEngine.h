/*
  ==============================================================================

    SequencerEngine.h

    Playback engine for StringSequencer. Tracks a looping playhead in
    quarter-note beats, fires onBlockEnter / onBlockExit callbacks when the
    playhead crosses block boundaries, and handles DAW-sync (setPositionBeats).

    Design constraints:
      - No JUCE dependency (plain C++17).
      - Intended to be driven from the audio thread; the StringSequencer is
        passed by const-ref on each advance() so the caller can hold an
        external lock while calling.
      - Callbacks fire synchronously from advance() / start() / setPositionBeats().

  ==============================================================================
*/

#pragma once

#include <cmath>
#include <functional>
#include "StringSequencer.h"

namespace fxme
{

struct EngineCallbacks
{
    std::function<void (int blockId, const std::string& content)> onBlockEnter;
    std::function<void (int blockId)>                             onBlockExit;
};

class SequencerEngine
{
public:
    explicit SequencerEngine (EngineCallbacks cbs) : callbacks_ (std::move (cbs)) {}

    /** By default, blocks with empty content are treated as unassigned and
        never fire callbacks. Consumers whose blocks are meaningful even
        without content (e.g. the content is an optional annotation) can opt
        in to entering them too. */
    void setEnterEmptyBlocks (bool shouldEnter) noexcept { enterEmptyBlocks_ = shouldEnter; }

    // ---- transport ---------------------------------------------------------

    /** Start playback. If the playhead is currently inside a block, fires
        onBlockEnter immediately. */
    void start (const StringSequencer& seq)
    {
        if (playing_) return;
        playing_ = true;
        const double ssb = seq.getStepSizeBeats();
        if (ssb > 0.0)
        {
            const int step = static_cast<int> (playheadBeats_ / ssb);
            const SeqBlock* b = seq.blockAt (step);
            if (b && (enterEmptyBlocks_ || ! b->content.empty()))
                enterBlock (b->id, b->content);
        }
    }

    /** Stop playback. If a block is active, fires onBlockExit. */
    void stop()
    {
        if (! playing_) return;
        exitCurrentBlock();
        playing_ = false;
    }

    bool   isPlaying()     const noexcept { return playing_; }
    double playheadBeats() const noexcept { return playheadBeats_; }
    int    activeBlockId() const noexcept { return activeBlockId_; }

    double playheadStep (const StringSequencer& seq) const noexcept
    {
        const double ssb = seq.getStepSizeBeats();
        return ssb > 0.0 ? playheadBeats_ / ssb : 0.0;
    }

    // ---- audio-thread advance ----------------------------------------------

    /** Advance the playhead by `deltaBeats` (quarter notes). Loops at pattern
        length; fires enter/exit callbacks for every block boundary crossed. */
    void advance (double deltaBeats, const StringSequencer& seq)
    {
        if (! playing_ || deltaBeats <= 0.0) return;
        const double patLen = seq.getPatternLengthBeats();
        const double ssb    = seq.getStepSizeBeats();
        if (patLen <= 0.0 || ssb <= 0.0) return;

        const double rawNew = playheadBeats_ + deltaBeats;
        const bool   looped = rawNew >= patLen;
        const double newBeats = std::fmod (rawNew, patLen);

        const int oldStep = static_cast<int> (playheadBeats_ / ssb);
        const int newStep = static_cast<int> (newBeats / ssb);
        const int numSteps = seq.getNumSteps();

        if (looped)
        {
            // Process step transitions up to the end of the pattern, then wrap.
            processSteps (oldStep + 1, numSteps, seq, ssb);
            exitCurrentBlock();  // loop boundary forces an exit for anything still active
            processSteps (0, newStep + 1, seq, ssb);
        }
        else if (newStep > oldStep)
        {
            processSteps (oldStep + 1, newStep + 1, seq, ssb);
        }

        playheadBeats_ = newBeats;
    }

    /** Set the playhead to an absolute beat position (for DAW sync).
        Fires callbacks if the position moves across block boundaries. */
    void setPositionBeats (double beats, const StringSequencer& seq)
    {
        if (! playing_) return;
        const double patLen = seq.getPatternLengthBeats();
        const double ssb    = seq.getStepSizeBeats();
        if (patLen <= 0.0 || ssb <= 0.0) return;

        beats = std::fmod (beats, patLen);
        if (beats < 0.0) beats += patLen;

        const int newStep = static_cast<int> (beats / ssb);

        // Exit active block if the new position is outside it.
        if (activeBlockId_ >= 0)
        {
            const SeqBlock* ab = seq.blockById (activeBlockId_);
            if (! ab || newStep < ab->startStep || newStep >= ab->endStep)
                exitCurrentBlock();
        }

        // Enter a block if we landed inside one and nothing is active.
        if (activeBlockId_ < 0)
        {
            const SeqBlock* nb = seq.blockAt (newStep);
            if (nb && (enterEmptyBlocks_ || ! nb->content.empty()))
                enterBlock (nb->id, nb->content);
        }

        playheadBeats_ = beats;
    }

    /** Like setPositionBeats, but always exits the active block and re-enters
        the block at the new position — even when it is the same block. Use
        for host-transport jumps (loop wraps, relocates) where re-entering
        matters (e.g. per-pass random draws keyed on the loop iteration);
        setPositionBeats keeps a block active when the jump lands inside it. */
    void relocate (double beats, const StringSequencer& seq)
    {
        if (! playing_) return;
        const double patLen = seq.getPatternLengthBeats();
        const double ssb    = seq.getStepSizeBeats();
        if (patLen <= 0.0 || ssb <= 0.0) return;

        exitCurrentBlock();

        beats = std::fmod (beats, patLen);
        if (beats < 0.0) beats += patLen;
        playheadBeats_ = beats;

        const SeqBlock* b = seq.blockAt (static_cast<int> (beats / ssb));
        if (b && (enterEmptyBlocks_ || ! b->content.empty()))
            enterBlock (b->id, b->content);
    }

    /** Reset the playhead to 0 (without firing callbacks). */
    void reset() { playheadBeats_ = 0.0; activeBlockId_ = -1; }

private:
    EngineCallbacks callbacks_;
    double playheadBeats_    = 0.0;
    bool   playing_          = false;
    bool   enterEmptyBlocks_ = false;
    int    activeBlockId_    = -1;

    void enterBlock (int id, const std::string& content)
    {
        activeBlockId_ = id;
        if (callbacks_.onBlockEnter)
            callbacks_.onBlockEnter (id, content);
    }

    void exitCurrentBlock()
    {
        if (activeBlockId_ < 0) return;
        if (callbacks_.onBlockExit)
            callbacks_.onBlockExit (activeBlockId_);
        activeBlockId_ = -1;
    }

    /** Process step transitions for step indices in [from, from+count).
        For each step: exit any block that ends there, then enter any block
        that starts there. */
    void processSteps (int from, int toExclusive, const StringSequencer& seq, double /*ssb*/)
    {
        for (int s = from; s < toExclusive; ++s)
        {
            // Exit the active block if this step is outside it — because it
            // ends here, or because it was moved/resized away from under the
            // playhead (message-thread edits while playing).
            if (activeBlockId_ >= 0)
            {
                const SeqBlock* ab = seq.blockById (activeBlockId_);
                if (! ab || s < ab->startStep || s >= ab->endStep)
                    exitCurrentBlock();
            }

            // Enter the block that starts at this step (if any; unassigned
            // blocks — empty content — are skipped unless enterEmptyBlocks_).
            if (activeBlockId_ < 0)
            {
                for (const auto& b : seq.blocks())
                {
                    if (b.startStep > s) break;
                    if (b.startStep == s && (enterEmptyBlocks_ || ! b.content.empty()))
                    {
                        enterBlock (b.id, b.content);
                        break;
                    }
                }
            }
        }
    }
};

} // namespace fxme
