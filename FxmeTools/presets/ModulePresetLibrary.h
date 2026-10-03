/*
  ==============================================================================

    ModulePresetLibrary.h

    The presets of one kind of module: a part of a plugin that has presets of
    its own (an FxmeFX effect, inside its own plugin or inside a bigger one;
    a Dede spectral region). See doc/local-presets-plan.md.

    The library is what does not depend on a particular instance of the
    module: its name, its folder of user presets, its factory presets and
    the two lists. Instances are ModulePresetTargets, which load and save
    through it; a plugin with several instances of a module (Dede's eight
    regions) has one library for all of them, so a preset saved from one
    shows up in every other's list at once (the library broadcasts a change
    message; the targets listen).

    Several plugin instances, or several plugins, share the folder on disk.
    Nothing watches it: rescan() before showing a list (the preset bar's
    browser and the targets' next / previous do).

    The file format, the same for user and factory presets:

        <FxmeModulePreset module="Compressor" version="1" name="Gentle Glue">
          <Param key="Attack" value="12.0"/>
          <Param key="Ratio"  value="2.0"/>
          <Extra> ... </Extra>          (optional, see ModulePresetTarget)
        </FxmeModulePreset>

    A key is a parameter ID without the instance's prefix, so one file fits
    every instance; a value is the parameter's real (denormalised) value.
    Files of another module, or of a newer version than the library's, are
    left out of the lists and refused by read().

    Factory presets are BinaryData "*_xml" resources whose root is
    FxmeModulePreset with this library's module name: a plugin can embed the
    factory presets of several modules, and its global presets, in one
    resource list; each library keeps only its own.

    Message thread only.

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "PresetBank.h"
#include <vector>

namespace fxme
{

/** The identifiers of the module preset file format, and of the bookkeeping
    ModulePresetTarget keeps in the APVTS state. */
namespace ModulePresetIds
{
    inline const juce::Identifier file    { "FxmeModulePreset" };
    inline const juce::Identifier module  { "module" };
    inline const juce::Identifier version { "version" };
    inline const juce::Identifier name    { "name" };
    inline const juce::Identifier param   { "Param" };
    inline const juce::Identifier key     { "key" };
    inline const juce::Identifier value   { "value" };
    inline const juce::Identifier extra   { "Extra" };

    /** In the APVTS state: <ModulePresets><Target id="<idPrefix>" name="..."
        factory="0|1"/>...</ModulePresets>. PresetManager does not count
        changes to this subtree as edits of the global preset. */
    inline const juce::Identifier state   { "ModulePresets" };
    inline const juce::Identifier target  { "Target" };
    inline const juce::Identifier id      { "id" };
    inline const juce::Identifier factory { "factory" };
}

class ModulePresetLibrary : public juce::ChangeBroadcaster
{
public:
    /** Signature of BinaryData::getNamedResource. */
    using ResourceProvider = const char* (*) (const char*, int&);

    /** `moduleName` names the kind of module in the files ("Compressor",
        "SpectralRegion"); `formatVersion` is its own format version (start
        at 1). The user folder is usually
        PresetManager::getModulePresetDirectory (moduleName). Pass the
        BinaryData accessors for factory presets, or leave them null. */
    ModulePresetLibrary (const juce::String& moduleName,
                         int formatVersion,
                         const juce::File& userDirectory,
                         const char* const* namedResourceList = nullptr,
                         int namedResourceListSize = 0,
                         ResourceProvider getNamedResource = nullptr);

    const juce::String& getModuleName() const noexcept         { return moduleName; }
    int getFormatVersion() const noexcept                       { return formatVersion; }

    const std::vector<Preset>& getFactoryPresets() const noexcept { return factoryPresets; }
    const std::vector<Preset>& getUserPresets() const noexcept    { return userPresets; }
    juce::File getUserDirectory() const                           { return userDir; }

    /** Re-reads the user folder (factory presets do not change). Broadcasts
        only when the list changed. */
    void rescan();

    /** A preset's file, parsed and checked (this module, a version no newer
        than the library's). Null when it cannot be read or is refused. */
    std::unique_ptr<juce::XmlElement> read (const Preset& preset) const;

    /** Whether `xml` is a preset this library can load. */
    bool accepts (const juce::XmlElement& xml) const;

    /** Saves `preset` (as made by ModulePresetTarget::capture) under `name`
        in the user folder, creating or overwriting it; stamps the module,
        version and name. Rescans and broadcasts. */
    bool write (const juce::String& name, const juce::XmlElement& preset);

    bool remove (const Preset& preset);
    bool rename (const Preset& preset, const juce::String& newName);

    /** Told when a user preset is renamed, so targets showing it can follow. */
    struct Listener
    {
        virtual ~Listener() = default;
        virtual void modulePresetRenamed (const juce::String& oldName, const juce::String& newName) = 0;
    };
    void addListener (Listener* l)       { listeners.add (l); }
    void removeListener (Listener* l)    { listeners.remove (l); }

private:
    void buildFactoryList (const char* const* list, int size);
    juce::File fileFor (const juce::String& name) const;

    const juce::String moduleName;
    const int formatVersion;
    const juce::File userDir;
    ResourceProvider getResource = nullptr;

    std::vector<Preset> factoryPresets, userPresets;
    juce::ListenerList<Listener> listeners;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModulePresetLibrary)
};

} // namespace fxme
