/*
  ==============================================================================

    ModulePresetTarget.h

    One instance of a module (see ModulePresetLibrary): the parameters of an
    APVTS whose IDs start with a prefix, loaded from and saved to the
    library's presets. It is a PresetBank, so PresetBarComponent and
    PresetComponent work on it as on a plugin's PresetManager.

        // processor members, in this order:
        fxme::ModulePresetLibrary regionLibrary { "SpectralRegion", 1,
            fxme::PresetManager::getModulePresetDirectory ("SpectralRegion"),
            BinaryData::namedResourceList, BinaryData::namedResourceListSize,
            BinaryData::getNamedResource };
        fxme::ModulePresetTarget region0 { regionLibrary, apvts, "rg0_" };

    Scope: the parameters whose ID starts with `idPrefix`, minus those the
    optional `include` filter turns down; its argument is the key, the ID
    without the prefix ("Attack" for "Main_Comp_Attack"). The scope is built
    once, at construction: the APVTS must already hold its parameters.

    Loading sets every parameter in scope: to the file's value for its key,
    or to its default when the file has no such key (a preset is a complete
    state of the module, so an older file, missing a parameter added since,
    still loads predictably). Keys the scope does not have are ignored. Each
    parameter is set as its own host gesture. Then onReadExtra.

    The current preset's name (and whether it is a factory one) lives in the
    APVTS state, in <ModulePresets><Target id="<idPrefix>" .../>, so it is
    saved with the host session and with global presets, and comes back
    with them. PresetManager does not count that bookkeeping as an edit; a
    module preset load still marks the global preset modified, through the
    parameters it changes.

    Dirty: set when a parameter in scope changes other than through a load
    (from any thread: automation arrives on the audio thread; the flag is
    atomic and the change message asynchronous). Cleared by a load or a
    save, and after the whole state is replaced (a global preset, a host
    session), as PresetManager does.

    Message thread, except the parameter listener.

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "PresetBank.h"
#include "ModulePresetLibrary.h"
#include <atomic>
#include <vector>

namespace fxme
{

class ModulePresetTarget : public PresetBank,
                           private juce::ChangeListener,
                           private juce::AudioProcessorValueTreeState::Listener,
                           private juce::ValueTree::Listener,
                           private ModulePresetLibrary::Listener
{
public:
    ModulePresetTarget (ModulePresetLibrary& library,
                        juce::AudioProcessorValueTreeState& apvts,
                        const juce::String& idPrefix,
                        std::function<bool (const juce::String& key)> include = {});
    ~ModulePresetTarget() override;

    const juce::String& getIdPrefix() const noexcept      { return prefix; }
    ModulePresetLibrary& getLibrary() const noexcept      { return library; }

    /** The parameters in scope (for a consumer that walks them). */
    const std::vector<juce::RangedAudioParameter*>& getParameters() const noexcept { return params; }

    //==========================================================================
    // Non-parameter settings travel in the file's <Extra> element.

    /** At save: add to `extra` (it is written only when not left empty). */
    std::function<void (juce::XmlElement& extra)> onWriteExtra;

    /** At load, after the parameters: the file's <Extra>, or null. */
    std::function<void (const juce::XmlElement* extra)> onReadExtra;

    //==========================================================================
    /** The instance as a preset, in memory (no name). */
    std::unique_ptr<juce::XmlElement> capture() const;

    /** Loads a preset from memory (e.g. a clipboard); the current preset
        becomes `name` (empty: none). False, and nothing changed, when
        `preset` is not one of this library's. */
    bool apply (const juce::XmlElement& preset, const juce::String& name = {}, bool isFactory = false);

    /** Sets what the instance shows as its current preset without loading
        anything: for a consumer that copied an instance's parameters by
        other means (Dede's Ctrl-drag copy of a region), or reset them
        (empty name: none). */
    void setCurrentPreset (const juce::String& name, bool isFactory, bool isDirty);

    //==========================================================================
    // PresetBank
    const std::vector<Preset>& getFactoryPresets() const override  { return library.getFactoryPresets(); }
    const std::vector<Preset>& getUserPresets() const override     { return library.getUserPresets(); }
    juce::File getUserPresetDirectory() const override             { return library.getUserDirectory(); }
    void rescanUserPresets() override                              { library.rescan(); }

    bool loadPreset (const Preset& preset) override;
    bool loadNext() override;
    bool loadPrevious() override;

    bool saveUserPreset (const juce::String& name) override;
    bool deleteUserPreset (const Preset& preset) override;
    bool renameUserPreset (const Preset& preset, const juce::String& newName) override;

    juce::String getCurrentPresetName() const override;
    bool currentPresetIsFactory() const override;
    bool isDirty() const override                                  { return dirty.load(); }

private:
    juce::ValueTree targetState() const;            // invalid when absent
    juce::ValueTree targetState (bool create);
    void setName (const juce::String& name, bool isFactory);
    void clearDirtyNowAndLater();

    void changeListenerCallback (juce::ChangeBroadcaster*) override;            // the library
    void parameterChanged (const juce::String& parameterID, float newValue) override;
    void valueTreeRedirected (juce::ValueTree&) override;
    void modulePresetRenamed (const juce::String& oldName, const juce::String& newName) override;

    ModulePresetLibrary& library;
    juce::AudioProcessorValueTreeState& apvts;
    const juce::String prefix;

    std::vector<juce::RangedAudioParameter*> params;
    juce::StringArray keys;                         // params[i]'s key

    std::atomic<bool> dirty { false };
    std::atomic<bool> loading { false };

    JUCE_DECLARE_WEAK_REFERENCEABLE (ModulePresetTarget)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModulePresetTarget)
};

} // namespace fxme
