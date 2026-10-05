/*
  ------------------------------------------------------------------------------
    AbComparison.h

    A/B comparison of a plugin's whole state: two slots, A and B, of the
    APVTS state. Switching to the other slot stores the current state in the
    one being left and restores the other, so each side keeps its own edits.
    The B slot starts as a copy of whatever A is the first time it is
    selected. copyToOther() overwrites the slot not in use with the current
    state (A -> B, or B -> A).

    The current preset's name lives in the APVTS state (PresetManager), so it
    follows each side. The slots are not saved with the session: A/B is a
    listening aid, not part of the settings.

    Owned by the processor, so the slots survive the editor being closed.
    Message thread only. AbCompareBar is its widget.

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>

namespace fxme
{

class AbComparison : public juce::ChangeBroadcaster
{
public:
    explicit AbComparison (juce::AudioProcessorValueTreeState& stateToCompare)
        : apvts (stateToCompare) {}

    /** 0 for A, 1 for B. */
    int getActiveSlot() const noexcept { return active; }

    /** Makes `slot` (0 or 1) the active one. */
    void select (int slot)
    {
        slot = slot != 0 ? 1 : 0;
        if (slot == active)
            return;

        slots[(size_t) active] = apvts.copyState();
        if (slots[(size_t) slot].isValid())
            apvts.replaceState (slots[(size_t) slot].createCopy());

        active = slot;
        sendChangeMessage();
    }

    void toggle() { select (1 - active); }

    /** Copies the current state into the slot not in use. */
    void copyToOther()
    {
        slots[(size_t) (1 - active)] = apvts.copyState();
        sendChangeMessage();
    }

    /** Forgets both slots and goes back to A (e.g. after a session load). */
    void reset()
    {
        slots[0] = slots[1] = {};
        active = 0;
        sendChangeMessage();
    }

private:
    juce::AudioProcessorValueTreeState& apvts;
    std::array<juce::ValueTree, 2> slots;
    int active = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AbComparison)
};

} // namespace fxme
