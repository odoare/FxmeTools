/*
  ==============================================================================

    PresetManager.cpp

  ==============================================================================
*/

#include "PresetManager.h"

namespace fxme
{

const juce::Identifier PresetManager::presetNameProperty      ("presetName");
const juce::Identifier PresetManager::presetIsFactoryProperty ("presetIsFactory");

PresetManager::PresetManager (juce::AudioProcessorValueTreeState& stateToManage,
                              const juce::File& userPresetDirectory,
                              const char* const* namedResourceList,
                              int namedResourceListSize,
                              ResourceProvider getNamedResource)
    : apvts (stateToManage),
      userDir (userPresetDirectory),
      getResource (getNamedResource)
{
    buildFactoryList (namedResourceList, namedResourceListSize);
    rescanUserPresets();
    apvts.state.addListener (this);
}

PresetManager::~PresetManager()
{
    apvts.state.removeListener (this);
}

juce::File PresetManager::getDefaultUserPresetDirectory (const juce::String& productName,
                                                         const juce::String& subProductName)
{
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    dir = dir.getChildFile ("Application Support");
   #endif
    dir = dir.getChildFile (productName);
    if (subProductName.isNotEmpty())
        dir = dir.getChildFile (subProductName);
    return dir.getChildFile ("Presets");
}

//==============================================================================
void PresetManager::buildFactoryList (const char* const* list, int size)
{
    factoryPresets.clear();

    if (list == nullptr || getResource == nullptr)
        return;

    for (int i = 0; i < size; ++i)
    {
        const juce::String res (list[i]);
        if (! res.endsWith ("_xml"))
            continue;

        int dataSize = 0;
        const char* data = getResource (res.toRawUTF8(), dataSize);
        if (data == nullptr)
            continue;

        auto xml = juce::XmlDocument::parse (juce::String::fromUTF8 (data, dataSize));
        if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
            continue;   // some other embedded XML asset, not a preset

        // Display name: the embedded one if present, else derived from the
        // BinaryData symbol ("Cool_Preset_xml" → "Cool Preset").
        auto name = xml->getStringAttribute (presetNameProperty.toString());
        if (name.isEmpty())
            name = res.dropLastCharacters (4).replace ("_", " ");

        factoryPresets.push_back ({ name, true, {}, res });
    }
}

void PresetManager::rescanUserPresets()
{
    userPresets.clear();

    auto files = userDir.findChildFiles (juce::File::findFiles, false, "*.xml");
    files.sort();

    for (const auto& f : files)
    {
        auto xml = juce::XmlDocument::parse (f);
        if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
            continue;

        auto name = xml->getStringAttribute (presetNameProperty.toString());
        if (name.isEmpty())
            name = f.getFileNameWithoutExtension();

        userPresets.push_back ({ name, false, f, {} });
    }
}

//==============================================================================
bool PresetManager::loadPreset (const Preset& preset)
{
    std::unique_ptr<juce::XmlElement> xml;

    if (preset.isFactory)
    {
        if (getResource == nullptr)
            return false;
        int dataSize = 0;
        if (const char* data = getResource (preset.resourceName.toRawUTF8(), dataSize))
            xml = juce::XmlDocument::parse (juce::String::fromUTF8 (data, dataSize));
    }
    else
    {
        xml = juce::XmlDocument::parse (preset.file);
    }

    return xml != nullptr && applyStateXml (*xml, preset);
}

bool PresetManager::loadFactoryPreset (int index)
{
    return juce::isPositiveAndBelow (index, (int) factoryPresets.size())
        && loadPreset (factoryPresets[(size_t) index]);
}

bool PresetManager::loadUserPreset (int index)
{
    return juce::isPositiveAndBelow (index, (int) userPresets.size())
        && loadPreset (userPresets[(size_t) index]);
}

bool PresetManager::loadNext()     { return step (+1); }
bool PresetManager::loadPrevious() { return step (-1); }

bool PresetManager::step (int delta)
{
    const int numFactory = (int) factoryPresets.size();
    const int total      = numFactory + (int) userPresets.size();
    if (total == 0)
        return false;

    int current = -1;
    if (const int fi = getCurrentFactoryIndex(); fi >= 0)
        current = fi;
    else if (const int ui = getCurrentUserIndex(); ui >= 0)
        current = numFactory + ui;

    const int next = current < 0 ? (delta > 0 ? 0 : total - 1)
                                 : (current + delta + total) % total;

    return loadPreset (next < numFactory ? factoryPresets[(size_t) next]
                                         : userPresets[(size_t) (next - numFactory)]);
}

bool PresetManager::applyStateXml (const juce::XmlElement& xml, const Preset& preset)
{
    if (! xml.hasTagName (apvts.state.getType()))
        return false;

    auto newState = juce::ValueTree::fromXml (xml);
    newState.setProperty (presetNameProperty, preset.name, nullptr);
    newState.setProperty (presetIsFactoryProperty, preset.isFactory, nullptr);

    {
        const juce::ScopedValueSetter<bool> svs (suppressDirty, true);
        apvts.replaceState (newState);
        if (onAfterLoad)
            onAfterLoad();
    }

    dirty = false;
    sendChangeMessage();
    clearDirtyAsync();
    return true;
}

//==============================================================================
bool PresetManager::saveUserPreset (const juce::String& rawName)
{
    const auto name = rawName.trim();
    if (name.isEmpty())
        return false;

    if (! userDir.createDirectory().wasOk())
        return false;

    const auto file = userDir.getChildFile (juce::File::createLegalFileName (name))
                             .withFileExtension ("xml");

    {
        const juce::ScopedValueSetter<bool> svs (suppressDirty, true);
        if (onBeforeSave)
            onBeforeSave();
        apvts.state.setProperty (presetNameProperty, name, nullptr);
        apvts.state.setProperty (presetIsFactoryProperty, false, nullptr);
    }

    auto xml = apvts.copyState().createXml();
    if (xml == nullptr || ! xml->writeTo (file))
        return false;

    rescanUserPresets();
    dirty = false;
    sendChangeMessage();
    return true;
}

bool PresetManager::deleteUserPreset (const Preset& preset)
{
    if (preset.isFactory || ! preset.file.existsAsFile())
        return false;

    if (! preset.file.moveToTrash() && ! preset.file.deleteFile())
        return false;

    rescanUserPresets();
    sendChangeMessage();
    return true;
}

bool PresetManager::renameUserPreset (const Preset& preset, const juce::String& rawNewName)
{
    const auto newName = rawNewName.trim();
    if (preset.isFactory || newName.isEmpty() || ! preset.file.existsAsFile())
        return false;

    auto xml = juce::XmlDocument::parse (preset.file);
    if (xml == nullptr)
        return false;

    xml->setAttribute (presetNameProperty.toString(), newName);

    const auto newFile = userDir.getChildFile (juce::File::createLegalFileName (newName))
                                .withFileExtension ("xml");
    if (! xml->writeTo (newFile))
        return false;
    if (newFile != preset.file)
        preset.file.deleteFile();

    // Keep the displayed name in sync if the renamed preset is the current one.
    if (! currentPresetIsFactory() && getCurrentPresetName() == preset.name)
    {
        const juce::ScopedValueSetter<bool> svs (suppressDirty, true);
        apvts.state.setProperty (presetNameProperty, newName, nullptr);
    }

    rescanUserPresets();
    sendChangeMessage();
    return true;
}

//==============================================================================
juce::String PresetManager::getCurrentPresetName() const
{
    return apvts.state.getProperty (presetNameProperty).toString();
}

bool PresetManager::currentPresetIsFactory() const
{
    return (bool) apvts.state.getProperty (presetIsFactoryProperty, false);
}

int PresetManager::getCurrentFactoryIndex() const
{
    if (! currentPresetIsFactory())
        return -1;
    const auto name = getCurrentPresetName();
    for (size_t i = 0; i < factoryPresets.size(); ++i)
        if (factoryPresets[i].name == name)
            return (int) i;
    return -1;
}

int PresetManager::getCurrentUserIndex() const
{
    if (currentPresetIsFactory())
        return -1;
    const auto name = getCurrentPresetName();
    for (size_t i = 0; i < userPresets.size(); ++i)
        if (userPresets[i].name == name)
            return (int) i;
    return -1;
}

//==============================================================================
void PresetManager::markDirty()
{
    if (suppressDirty)
        return;
    if (! dirty.exchange (true))
        sendChangeMessage();
}

void PresetManager::clearDirtyAsync()
{
    // After replaceState() the APVTS flushes parameter values back into the
    // tree from the message queue; those writes would re-flag dirty. Clear it
    // again once the queue has drained the load.
    juce::MessageManager::callAsync ([weak = juce::WeakReference<PresetManager> (this)]
    {
        if (auto* pm = weak.get())
            if (pm->dirty.exchange (false))
                pm->sendChangeMessage();
    });
}

void PresetManager::valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier& property)
{
    if (property != presetNameProperty && property != presetIsFactoryProperty)
        markDirty();
}

void PresetManager::valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&)        { markDirty(); }
void PresetManager::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) { markDirty(); }
void PresetManager::valueTreeChildOrderChanged (juce::ValueTree&, int, int)         { markDirty(); }

void PresetManager::valueTreeRedirected (juce::ValueTree&)
{
    // Wholesale state replacement outside a preset load (e.g. the host
    // restoring a session) is treated as a load, not an edit.
    if (! suppressDirty)
    {
        dirty = false;
        sendChangeMessage();
    }
}

} // namespace fxme
