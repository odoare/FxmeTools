/*
  ==============================================================================

    ModulePresetTarget.cpp

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ==============================================================================
*/

#include "ModulePresetTarget.h"

namespace fxme
{

ModulePresetTarget::ModulePresetTarget (ModulePresetLibrary& libraryToUse,
                                        juce::AudioProcessorValueTreeState& stateToUse,
                                        const juce::String& idPrefix,
                                        std::function<bool (const juce::String& key)> include)
    : library (libraryToUse), apvts (stateToUse), prefix (idPrefix)
{
    for (auto* p : apvts.processor.getParameters())
    {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p);
        if (ranged == nullptr || ! ranged->getParameterID().startsWith (prefix))
            continue;

        const auto key = ranged->getParameterID().substring (prefix.length());
        if (key.isEmpty() || (include != nullptr && ! include (key)))
            continue;

        params.push_back (ranged);
        keys.add (key);
        apvts.addParameterListener (ranged->getParameterID(), this);
    }

    library.addChangeListener (this);
    library.addListener (this);
    apvts.state.addListener (this);
}

ModulePresetTarget::~ModulePresetTarget()
{
    apvts.state.removeListener (this);
    library.removeListener (this);
    library.removeChangeListener (this);
    for (auto* p : params)
        apvts.removeParameterListener (p->getParameterID(), this);
}

//==============================================================================
std::unique_ptr<juce::XmlElement> ModulePresetTarget::capture() const
{
    auto xml = std::make_unique<juce::XmlElement> (ModulePresetIds::file);
    xml->setAttribute (ModulePresetIds::module.toString(), library.getModuleName());
    xml->setAttribute (ModulePresetIds::version.toString(), library.getFormatVersion());

    for (size_t i = 0; i < params.size(); ++i)
    {
        auto* p = params[i];
        auto* e = xml->createNewChildElement (ModulePresetIds::param.toString());
        e->setAttribute (ModulePresetIds::key.toString(), keys[(int) i]);
        e->setAttribute (ModulePresetIds::value.toString(), p->convertFrom0to1 (p->getValue()));
    }

    if (onWriteExtra != nullptr)
    {
        auto extra = std::make_unique<juce::XmlElement> (ModulePresetIds::extra);
        onWriteExtra (*extra);
        if (extra->getNumChildElements() > 0 || extra->getNumAttributes() > 0
            || extra->getAllSubText().isNotEmpty())
            xml->addChildElement (extra.release());
    }

    return xml;
}

bool ModulePresetTarget::apply (const juce::XmlElement& preset, const juce::String& name, bool isFactory)
{
    if (! library.accepts (preset))
        return false;

    juce::StringPairArray values;
    for (auto* e : preset.getChildWithTagNameIterator (ModulePresetIds::param.toString()))
        values.set (e->getStringAttribute (ModulePresetIds::key.toString()),
                    e->getStringAttribute (ModulePresetIds::value.toString()));

    {
        // The parameter listener runs synchronously inside each change.
        loading = true;
        for (size_t i = 0; i < params.size(); ++i)
        {
            auto* p = params[i];
            const auto& key = keys[(int) i];
            const float normalised = values.containsKey (key)
                                         ? p->convertTo0to1 (values[key].getFloatValue())
                                         : p->getDefaultValue();
            p->beginChangeGesture();
            p->setValueNotifyingHost (normalised);
            p->endChangeGesture();
        }

        if (onReadExtra != nullptr)
            onReadExtra (preset.getChildByName (ModulePresetIds::extra));
        loading = false;
    }

    setName (name, isFactory);
    clearDirtyNowAndLater();
    sendChangeMessage();
    return true;
}

void ModulePresetTarget::setCurrentPreset (const juce::String& name, bool isFactory, bool isDirty)
{
    setName (name, isFactory);
    if (isDirty)
        dirty = true;
    else
        clearDirtyNowAndLater();
    sendChangeMessage();
}

//==============================================================================
bool ModulePresetTarget::loadPreset (const Preset& preset)
{
    auto xml = library.read (preset);
    return xml != nullptr && apply (*xml, preset.name, preset.isFactory);
}

bool ModulePresetTarget::loadNext()
{
    library.rescan();   // another instance may have saved since
    return step (+1);
}

bool ModulePresetTarget::loadPrevious()
{
    library.rescan();
    return step (-1);
}

bool ModulePresetTarget::saveUserPreset (const juce::String& rawName)
{
    const auto name = rawName.trim();
    auto xml = capture();
    if (xml == nullptr || ! library.write (name, *xml))
        return false;

    setName (name, false);
    clearDirtyNowAndLater();
    sendChangeMessage();
    return true;
}

bool ModulePresetTarget::deleteUserPreset (const Preset& preset)
{
    return library.remove (preset);
}

bool ModulePresetTarget::renameUserPreset (const Preset& preset, const juce::String& newName)
{
    // The library tells every target showing it (this one included).
    return library.rename (preset, newName);
}

//==============================================================================
juce::ValueTree ModulePresetTarget::targetState() const
{
    return apvts.state.getChildWithName (ModulePresetIds::state)
                      .getChildWithProperty (ModulePresetIds::id, prefix);
}

juce::ValueTree ModulePresetTarget::targetState (bool create)
{
    auto t = targetState();
    if (t.isValid() || ! create)
        return t;

    auto all = apvts.state.getOrCreateChildWithName (ModulePresetIds::state, nullptr);
    t = juce::ValueTree (ModulePresetIds::target);
    t.setProperty (ModulePresetIds::id, prefix, nullptr);
    all.appendChild (t, nullptr);
    return t;
}

juce::String ModulePresetTarget::getCurrentPresetName() const
{
    return targetState().getProperty (ModulePresetIds::name).toString();
}

bool ModulePresetTarget::currentPresetIsFactory() const
{
    return (bool) targetState().getProperty (ModulePresetIds::factory, false);
}

void ModulePresetTarget::setName (const juce::String& name, bool isFactory)
{
    if (name.isEmpty())
    {
        if (auto t = targetState(); t.isValid())
            t.getParent().removeChild (t, nullptr);
        return;
    }

    auto t = targetState (true);
    t.setProperty (ModulePresetIds::name, name, nullptr);
    t.setProperty (ModulePresetIds::factory, isFactory, nullptr);
}

void ModulePresetTarget::clearDirtyNowAndLater()
{
    dirty = false;

    // Whatever the APVTS still does with the values it was just given (on
    // a state replacement it sets them from its own redirect callback,
    // whose order against this one is not specified) must not count as an
    // edit: clear again once the message queue has caught up.
    juce::MessageManager::callAsync ([weak = juce::WeakReference<ModulePresetTarget> (this)]
    {
        if (auto* t = weak.get())
            if (t->dirty.exchange (false))
                t->sendChangeMessage();
    });
}

//==============================================================================
void ModulePresetTarget::changeListenerCallback (juce::ChangeBroadcaster*)
{
    sendChangeMessage();   // the library's lists changed
}

void ModulePresetTarget::parameterChanged (const juce::String&, float)
{
    if (loading)
        return;
    if (! dirty.exchange (true))
        sendChangeMessage();
}

void ModulePresetTarget::valueTreeRedirected (juce::ValueTree&)
{
    // The whole state was replaced (a global preset, a host session): it
    // brings its own current module presets; their values are as saved.
    clearDirtyNowAndLater();
    sendChangeMessage();
}

void ModulePresetTarget::modulePresetRenamed (const juce::String& oldName, const juce::String& newName)
{
    if (! currentPresetIsFactory() && getCurrentPresetName() == oldName && oldName.isNotEmpty())
    {
        setName (newName, false);
        sendChangeMessage();
    }
}

} // namespace fxme
