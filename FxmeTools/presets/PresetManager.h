/*
  ==============================================================================

    PresetManager.h

    Preset management for FX-Mechanics plugins. Two preset banks are exposed:

      * Factory presets — APVTS state XML files embedded in the plugin binary
        (via juce_add_binary_data). The manager is handed the BinaryData
        accessors and keeps every "*_xml" resource whose root tag matches the
        APVTS state type, so other embedded XML assets are ignored.

      * User presets — plain XML files in a per-product folder under the
        platform user-data directory. They can be created, overwritten,
        renamed and deleted at runtime. The FX-Mechanics layout is
        getVendorPresetDirectory() (<user data>/FX-Mechanics/<Plugin>/Presets);
        a plugin moving there from an older folder (e.g. its
        getDefaultUserPresetDirectory()) calls importLegacyUserPresets() once
        after construction, so its users keep their presets.

    Both banks share the same file format: the APVTS state XML, as written by
    apvts.copyState().createXml(). The current preset name (and whether it is
    a factory preset) is stored as a property on the APVTS state itself, so it
    persists in host sessions and round-trips through preset files.

    The manager broadcasts a change message whenever the preset lists, the
    current preset or the dirty flag change; GUI code (e.g. PresetComponent)
    listens and refreshes itself. It is a PresetBank, the interface the
    preset widgets work on (so they also serve module presets).

    Typical processor setup:

        MyProcessor()
            : apvts (*this, nullptr, "Parameters", createLayout()),
              presetManager (apvts,
                             fxme::PresetManager::getVendorPresetDirectory ("MyPlugin"),
                             BinaryData::namedResourceList,
                             BinaryData::namedResourceListSize,
                             BinaryData::getNamedResource)
        {
            // Only for a plugin that kept its presets elsewhere before:
            presetManager.importLegacyUserPresets (
                fxme::PresetManager::getDefaultUserPresetDirectory ("MyPlugin"));
        }

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "PresetBank.h"

namespace fxme
{

class PresetManager : public PresetBank,
                      private juce::ValueTree::Listener
{
public:
    // PresetManager::Preset is fxme::Preset (inherited alias), as before.

    // Signature of BinaryData::getNamedResource.
    using ResourceProvider = const char* (*) (const char*, int&);

    // Pass the BinaryData accessors to enable factory presets; leave them
    // null for a user-presets-only manager.
    PresetManager (juce::AudioProcessorValueTreeState& stateToManage,
                   const juce::File& userPresetDirectory,
                   const char* const* namedResourceList = nullptr,
                   int namedResourceListSize = 0,
                   ResourceProvider getNamedResource = nullptr);
    ~PresetManager() override;

    //==========================================================================
    // Folders. <user data> is ~/.config on Linux, ~/Library/Application
    // Support on macOS, %APPDATA% on Windows.

    /** The vendor folder every FX-Mechanics plugin keeps its data under. */
    static constexpr const char* vendorFolderName = "FX-Mechanics";

    /** <user data>/FX-Mechanics/<pluginName>/Presets: a plugin's own (global)
        presets. */
    static juce::File getVendorPresetDirectory (const juce::String& pluginName);

    /** <user data>/FX-Mechanics/Modules/<moduleName>/Presets: the presets of
        a module shared between plugins (see doc/local-presets-plan.md). */
    static juce::File getModulePresetDirectory (const juce::String& moduleName);

    /** <user data>/<productName>[/<subProductName>]/Presets: the folder
        plugins used before the vendor layout. Unchanged, so a plugin still
        using it keeps finding its users' presets; one moving to the vendor
        folder passes it to importLegacyUserPresets(). */
    static juce::File getDefaultUserPresetDirectory (const juce::String& productName,
                                                     const juce::String& subProductName = {});

    /** Copies the user presets of an older folder into this manager's user
        folder, once: every *.xml there with no file of the same name here.
        Copies, never moves, so an older version of the plugin still installed
        keeps its own presets. A marker file left in the old folder
        (importMarkerFileName) stops it happening again, so a preset deleted
        here does not come back. Quiet on failure (nothing to import, or a
        folder that cannot be written: nothing happens, and the marker is not
        written, so it is tried again next time). Rescans and broadcasts when
        anything was copied. Message thread. Returns how many were copied. */
    int importLegacyUserPresets (const juce::File& legacyDirectory);

    /** The marker importLegacyUserPresets() leaves in an imported folder. */
    static constexpr const char* importMarkerFileName = ".imported-to-FX-Mechanics";

    //==========================================================================
    // Preset lists
    const std::vector<Preset>& getFactoryPresets() const noexcept override { return factoryPresets; }
    const std::vector<Preset>& getUserPresets()    const noexcept override { return userPresets; }
    juce::File getUserPresetDirectory() const override { return userDir; }
    void rescanUserPresets() override;

    //==========================================================================
    // Loading. loadFactoryPreset (index), loadUserPreset (index), loadNext()
    // and loadPrevious() (factory then user presets, wrapping around) come
    // from PresetBank.
    bool loadPreset (const Preset& preset) override;

    //==========================================================================
    // User bank management. Names are free text; the file name is a legalised
    // version of it, the display name round-trips via an XML attribute.
    bool saveUserPreset (const juce::String& name) override;   // creates or overwrites
    bool deleteUserPreset (const Preset& preset) override;
    bool renameUserPreset (const Preset& preset, const juce::String& newName) override;

    //==========================================================================
    // Side-state hooks, for processors that keep non-parameter data outside
    // apvts.state (extra ValueTree children merged in at save time only).
    // Both run on the message thread with the dirty tracking suppressed.

    /** Called just before a preset is written: merge any side state into
        apvts.state so it lands in the preset file. */
    std::function<void()> onBeforeSave;

    /** Called just before a loaded preset replaces the state, while the old
        state is still in place. A processor that keeps state a preset must
        not disturb (hardware selections, calibration, anything describing
        the installation rather than the settings) snapshots it here and puts
        it back in onAfterLoad. */
    std::function<void()> onBeforeLoad;

    /** Called right after a loaded preset's state has been applied
        (replaceState), before the change broadcast: rebuild whatever
        depends on the side state now inside apvts.state. */
    std::function<void()> onAfterLoad;

    //==========================================================================
    // Current preset info
    // (getCurrentFactoryIndex() and getCurrentUserIndex(), -1 when the
    // current preset is not of that kind, come from PresetBank.)
    juce::String getCurrentPresetName() const override;
    bool currentPresetIsFactory() const override;
    bool isDirty() const noexcept override { return dirty.load(); }   // state edited since last load/save

    // State properties used to persist the current preset identity.
    static const juce::Identifier presetNameProperty;       // "presetName"
    static const juce::Identifier presetIsFactoryProperty;  // "presetIsFactory"

private:
    void buildFactoryList (const char* const* list, int size);
    bool applyStateXml (const juce::XmlElement& xml, const Preset& preset);
    void markDirty();
    void clearDirtyAsync();

    // ValueTree::Listener — flags the state dirty on any edit. The listener
    // survives replaceState() (JUCE redirects it to the new tree).
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override;
    void valueTreeRedirected (juce::ValueTree&) override;

    juce::AudioProcessorValueTreeState& apvts;
    juce::File userDir;
    ResourceProvider getResource = nullptr;

    std::vector<Preset> factoryPresets, userPresets;
    std::atomic<bool> dirty { false };
    bool suppressDirty = false;

    JUCE_DECLARE_WEAK_REFERENCEABLE (PresetManager)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetManager)
};

} // namespace fxme
