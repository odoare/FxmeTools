/*
  ==============================================================================

    PresetBank.h

    What a preset browser needs from a bank of presets, whatever the presets
    cover: the whole plugin (PresetManager) or one part of it (a module
    preset target, see doc/local-presets-plan.md). PresetBarComponent and
    PresetComponent work on this interface, so they serve both.

    A bank has two lists, factory (read-only, usually embedded in the
    binary) and user (files in a folder), a current preset with a dirty
    flag, and broadcasts a change message whenever any of that changes.

    Implementers provide the pure virtual members. Stepping (loadNext,
    loadPrevious), loading by index and finding the current preset's index
    have default implementations written in terms of those; override them
    only to add to them (a rescan before stepping, say).

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include <vector>

namespace fxme
{

/** One entry of a bank's factory or user list. */
struct Preset
{
    juce::String name;
    bool isFactory = false;
    juce::File file;             // user presets only
    juce::String resourceName;   // factory presets only (BinaryData symbol, e.g. "Cool_Preset_xml")
};

class PresetBank : public juce::ChangeBroadcaster
{
public:
    using Preset = fxme::Preset;

    ~PresetBank() override = default;

    //==========================================================================
    // Lists
    virtual const std::vector<Preset>& getFactoryPresets() const = 0;
    virtual const std::vector<Preset>& getUserPresets() const = 0;
    virtual juce::File getUserPresetDirectory() const = 0;
    virtual void rescanUserPresets() = 0;

    //==========================================================================
    // Loading
    virtual bool loadPreset (const Preset& preset) = 0;

    virtual bool loadFactoryPreset (int index)
    {
        const auto& list = getFactoryPresets();
        return juce::isPositiveAndBelow (index, (int) list.size()) && loadPreset (list[(size_t) index]);
    }

    virtual bool loadUserPreset (int index)
    {
        const auto& list = getUserPresets();
        return juce::isPositiveAndBelow (index, (int) list.size()) && loadPreset (list[(size_t) index]);
    }

    /** Walk factory then user presets, wrapping around. From no current
        preset, next starts at the first and previous at the last. */
    virtual bool loadNext()     { return step (+1); }
    virtual bool loadPrevious() { return step (-1); }

    //==========================================================================
    // The user list. Names are free text; the file name is a legalised
    // version of it.
    virtual bool saveUserPreset (const juce::String& name) = 0;   // creates or overwrites
    virtual bool deleteUserPreset (const Preset& preset) = 0;
    virtual bool renameUserPreset (const Preset& preset, const juce::String& newName) = 0;

    //==========================================================================
    // The current preset
    virtual juce::String getCurrentPresetName() const = 0;
    virtual bool currentPresetIsFactory() const = 0;
    virtual bool isDirty() const = 0;   // edited since the last load or save

    /** -1 when the current preset is not a factory preset (or not found). */
    virtual int getCurrentFactoryIndex() const
    {
        return currentPresetIsFactory() ? indexOf (getFactoryPresets(), getCurrentPresetName()) : -1;
    }

    /** -1 when the current preset is not a user preset (or not found). */
    virtual int getCurrentUserIndex() const
    {
        return currentPresetIsFactory() ? -1 : indexOf (getUserPresets(), getCurrentPresetName());
    }

protected:
    bool step (int delta)
    {
        const auto& factory = getFactoryPresets();
        const auto& user    = getUserPresets();
        const int numFactory = (int) factory.size();
        const int total      = numFactory + (int) user.size();
        if (total == 0)
            return false;

        int current = -1;
        if (const int fi = getCurrentFactoryIndex(); fi >= 0)
            current = fi;
        else if (const int ui = getCurrentUserIndex(); ui >= 0)
            current = numFactory + ui;

        const int next = current < 0 ? (delta > 0 ? 0 : total - 1)
                                     : (current + delta + total) % total;

        return loadPreset (next < numFactory ? factory[(size_t) next]
                                             : user[(size_t) (next - numFactory)]);
    }

private:
    static int indexOf (const std::vector<Preset>& list, const juce::String& name)
    {
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].name == name)
                return (int) i;
        return -1;
    }
};

} // namespace fxme
