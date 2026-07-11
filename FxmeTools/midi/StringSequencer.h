/*
  ==============================================================================

    StringSequencer.h

    A generic, JUCE-free step sequencer whose "payload" per block is an
    arbitrary string (e.g. a chord ID in Neorix). Blocks live on an integer
    step grid, are always sorted by start step, and are guaranteed
    non-overlapping: every mutation method enforces this invariant.

  ==============================================================================
*/

#pragma once

#include <algorithm>
#include <string>
#include <vector>

namespace fxme
{

/** The available step-size values. */
enum class SeqStepSize { Sixteenth = 0, Eighth, Quarter, Half, Whole };

/** Converts a SeqStepSize to its duration in quarter-note beats (4/4 assumed). */
inline double stepSizeBeats (SeqStepSize s) noexcept
{
    switch (s)
    {
        case SeqStepSize::Sixteenth: return 0.25;
        case SeqStepSize::Eighth:    return 0.5;
        case SeqStepSize::Quarter:   return 1.0;
        case SeqStepSize::Half:      return 2.0;
        case SeqStepSize::Whole:     return 4.0;
    }
    return 0.25;
}

/** A single block on the rubber: integer step range [startStep, endStep),
    duration >= 1. `content` is the string payload; empty = unassigned. */
struct SeqBlock
{
    int         id        = -1;
    int         startStep = 0;
    int         endStep   = 1;   // exclusive
    std::string content;
};

/** Step sequencer data model. All modifications run on the message thread;
    the audio thread reads under an external lock (provided by the caller). */
class StringSequencer
{
public:
    void setStepSize (SeqStepSize s) { stepSize_ = s; }

    void setNumSteps (int n)
    {
        numSteps_ = std::max (1, std::min (64, n));
        blocks_.erase (std::remove_if (blocks_.begin(), blocks_.end(),
                           [this] (const SeqBlock& b) { return b.startStep >= numSteps_; }),
                       blocks_.end());
        for (auto& b : blocks_)
            b.endStep = std::min (b.endStep, numSteps_);
    }

    SeqStepSize getStepSize()           const noexcept { return stepSize_; }
    int         getNumSteps()           const noexcept { return numSteps_; }
    double      getStepSizeBeats()      const noexcept { return fxme::stepSizeBeats (stepSize_); }
    double      getPatternLengthBeats() const noexcept { return numSteps_ * getStepSizeBeats(); }

    /** Returns the new block id, or -1 if the range overlaps an existing block
        or the parameters are out of range. */
    int addBlock (int startStep, int durationSteps)
    {
        if (startStep < 0 || startStep >= numSteps_ || durationSteps < 1)
            return -1;
        const int endStep = std::min (startStep + durationSteps, numSteps_);
        for (const auto& b : blocks_)
            if (startStep < b.endStep && endStep > b.startStep)
                return -1;
        SeqBlock blk;
        blk.id        = nextId_++;
        blk.startStep = startStep;
        blk.endStep   = endStep;
        blocks_.push_back (blk);
        sortBlocks();
        return blk.id;
    }

    /** Adds a block with a caller-chosen id — for restoring saved state where
        block ids must survive the round-trip (e.g. when ids seed per-block
        random draws). Returns false if the id is taken/negative or the range
        is invalid/overlapping. Bumps the internal id counter past `id` so
        later addBlock() calls stay unique. */
    bool addBlockWithId (int id, int startStep, int durationSteps)
    {
        if (id < 0 || blockById (id) != nullptr)
            return false;
        if (startStep < 0 || startStep >= numSteps_ || durationSteps < 1)
            return false;
        const int endStep = std::min (startStep + durationSteps, numSteps_);
        for (const auto& b : blocks_)
            if (startStep < b.endStep && endStep > b.startStep)
                return false;
        SeqBlock blk;
        blk.id        = id;
        blk.startStep = startStep;
        blk.endStep   = endStep;
        blocks_.push_back (blk);
        sortBlocks();
        nextId_ = std::max (nextId_, id + 1);
        return true;
    }

    bool removeBlock (int id)
    {
        auto it = findById (id);
        if (it == blocks_.end()) return false;
        blocks_.erase (it);
        return true;
    }

    bool setContent (int id, const std::string& content)
    {
        auto it = findById (id);
        if (it == blocks_.end()) return false;
        it->content = content;
        return true;
    }

    bool clearContent (int id)
    {
        auto it = findById (id);
        if (it == blocks_.end()) return false;
        it->content.clear();
        return true;
    }

    /** Move a block's start step, clamped against the previous block's end.
        Minimum block duration is 1 step. Returns false if the block was not found. */
    bool moveBlockStart (int id, int newStart)
    {
        auto it = findById (id);
        if (it == blocks_.end()) return false;
        const int minS = prevEnd (it);
        const int maxS = it->endStep - 1;
        it->startStep = std::max (minS, std::min (maxS, newStart));
        sortBlocks();
        return true;
    }

    /** Move a block's end step (exclusive), clamped against the next block's start
        and the pattern end. Minimum block duration is 1 step. */
    bool moveBlockEnd (int id, int newEnd)
    {
        auto it = findById (id);
        if (it == blocks_.end()) return false;
        const int minE = it->startStep + 1;
        const int maxE = nextStart (it);
        it->endStep = std::max (minE, std::min (maxE, newEnd));
        return true;
    }

    const std::vector<SeqBlock>& blocks() const noexcept { return blocks_; }

    /** Returns a pointer to the block covering `step` (i.e. startStep <= step < endStep),
        or nullptr if no block covers that step. */
    const SeqBlock* blockAt (int step) const noexcept
    {
        for (const auto& b : blocks_)
        {
            if (b.startStep > step) break;
            if (step < b.endStep)  return &b;
        }
        return nullptr;
    }

    const SeqBlock* blockById (int id) const noexcept
    {
        for (const auto& b : blocks_)
            if (b.id == id) return &b;
        return nullptr;
    }

    /** Returns the maximum allowed endStep (exclusive) for a block that starts
        at `startStep`, optionally excluding a block (e.g. the one being resized). */
    int maxEnd (int startStep, int excludeId = -1) const noexcept
    {
        int limit = numSteps_;
        for (const auto& b : blocks_)
        {
            if (b.id == excludeId) continue;
            if (b.startStep >= startStep)
            {
                limit = std::min (limit, b.startStep);
                break;
            }
        }
        return limit;
    }

    /** Returns the minimum allowed startStep after `endStep`, optionally excluding
        a block. */
    int minStart (int afterEnd, int excludeId = -1) const noexcept
    {
        int floor = 0;
        for (const auto& b : blocks_)
        {
            if (b.id == excludeId) continue;
            if (b.endStep <= afterEnd)
                floor = std::max (floor, b.endStep);
        }
        return floor;
    }

    void clear() { blocks_.clear(); }

private:
    SeqStepSize           stepSize_ = SeqStepSize::Sixteenth;
    int                   numSteps_ = 16;
    int                   nextId_   = 0;
    std::vector<SeqBlock> blocks_;

    void sortBlocks()
    {
        std::sort (blocks_.begin(), blocks_.end(),
                   [] (const SeqBlock& a, const SeqBlock& b) { return a.startStep < b.startStep; });
    }

    using Iter = std::vector<SeqBlock>::iterator;

    Iter findById (int id) noexcept
    {
        return std::find_if (blocks_.begin(), blocks_.end(),
                             [id] (const SeqBlock& b) { return b.id == id; });
    }

    int prevEnd (Iter it) const noexcept
    {
        if (it == blocks_.begin()) return 0;
        return std::prev (it)->endStep;
    }

    int nextStart (Iter it) const noexcept
    {
        auto nx = std::next (it);
        return (nx == blocks_.end()) ? numSteps_ : nx->startStep;
    }
};

} // namespace fxme
