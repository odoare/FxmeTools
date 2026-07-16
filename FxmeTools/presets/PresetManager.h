/*
  ==============================================================================

    PresetManager.h

    Preset management for FX-Mechanics plugins. Two preset banks are exposed:

      * Factory presets — APVTS state XML files embedded in the plugin binary
        (via juce_add_binary_data). The manager is handed the BinaryData
        accessors and keeps every "*_xml" resource whose root tag matches the
        APVTS state type, so other embedded XML assets are ignored.

      * User presets — plain XML files in a per-product folder under the
        platform user-data directory (see getDefaultUserPresetDirectory()).
        They can be created, overwritten, renamed and deleted at runtime.

    Both banks share the same file format: the APVTS state XML, as written by
    apvts.copyState().createXml(). The current preset name (and whether it is
    a factory preset) is stored as a property on the APVTS state itself, so it
    persists in host sessions and round-trips through preset files.

    The manager broadcasts a change message whenever the preset lists, the
    current preset or the dirty flag change; GUI code (e.g. PresetComponent)
    listens and refreshes itself.

    Typical processor setup:

        MyProcessor()
            : apvts (*this, nullptr, "Parameters", createLayout()),
              presetManager (apvts,
                             fxme::PresetManager::getDefaultUserPresetDirectory ("MyPlugin"),
                             BinaryData::namedResourceList,
                             BinaryData::namedResourceListSize,
                             BinaryData::getNamedResource)
        {}

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

namespace fxme
{

class PresetManager : public juce::ChangeBroadcaster,
                      private juce::ValueTree::Listener
{
public:
    struct Preset
    {
        juce::String name;
        bool isFactory = false;
        juce::File file;             // user presets only
        juce::String resourceName;   // factory presets only (BinaryData symbol, e.g. "Cool_Preset_xml")
    };

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

    // <user-app-data>/<productName>[/<subProductName>]/Presets
    // (~/.config on Linux, ~/Library/Application Support on macOS, %APPDATA% on Windows)
    static juce::File getDefaultUserPresetDirectory (const juce::String& productName,
                                                     const juce::String& subProductName = {});

    //==========================================================================
    // Preset lists
    const std::vector<Preset>& getFactoryPresets() const noexcept { return factoryPresets; }
    const std::vector<Preset>& getUserPresets()    const noexcept { return userPresets; }
    juce::File getUserPresetDirectory() const { return userDir; }
    void rescanUserPresets();

    //==========================================================================
    // Loading
    bool loadPreset (const Preset& preset);
    bool loadFactoryPreset (int index);
    bool loadUserPreset (int index);
    bool loadNext();       // walks factory then user presets, wrapping around
    bool loadPrevious();

    //==========================================================================
    // User bank management. Names are free text; the file name is a legalised
    // version of it, the display name round-trips via an XML attribute.
    bool saveUserPreset (const juce::String& name);   // creates or overwrites
    bool deleteUserPreset (const Preset& preset);
    bool renameUserPreset (const Preset& preset, const juce::String& newName);

    //==========================================================================
    // Side-state hooks, for processors that keep non-parameter data outside
    // apvts.state (extra ValueTree children merged in at save time only).
    // Both run on the message thread with the dirty tracking suppressed.

    /** Called just before a preset is written: merge any side state into
        apvts.state so it lands in the preset file. */
    std::function<void()> onBeforeSave;

    /** Called right after a loaded preset's state has been applied
        (replaceState), before the change broadcast: rebuild whatever
        depends on the side state now inside apvts.state. */
    std::function<void()> onAfterLoad;

    //==========================================================================
    // Current preset info
    juce::String getCurrentPresetName() const;
    bool currentPresetIsFactory() const;
    int  getCurrentFactoryIndex() const;   // -1 if the current preset is not a factory preset
    int  getCurrentUserIndex() const;      // -1 if the current preset is not a user preset
    bool isDirty() const noexcept { return dirty.load(); }   // state edited since last load/save

    // State properties used to persist the current preset identity.
    static const juce::Identifier presetNameProperty;       // "presetName"
    static const juce::Identifier presetIsFactoryProperty;  // "presetIsFactory"

private:
    void buildFactoryList (const char* const* list, int size);
    bool applyStateXml (const juce::XmlElement& xml, const Preset& preset);
    bool step (int delta);
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
