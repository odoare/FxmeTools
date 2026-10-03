/*
  ==============================================================================

    ModulePresetLibrary.cpp

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ==============================================================================
*/

#include "ModulePresetLibrary.h"

namespace fxme
{

ModulePresetLibrary::ModulePresetLibrary (const juce::String& moduleNameToUse,
                                          int formatVersionToUse,
                                          const juce::File& userDirectory,
                                          const char* const* namedResourceList,
                                          int namedResourceListSize,
                                          ResourceProvider getNamedResource)
    : moduleName (moduleNameToUse),
      formatVersion (juce::jmax (1, formatVersionToUse)),
      userDir (userDirectory),
      getResource (getNamedResource)
{
    buildFactoryList (namedResourceList, namedResourceListSize);
    rescan();
}

//==============================================================================
bool ModulePresetLibrary::accepts (const juce::XmlElement& xml) const
{
    return xml.hasTagName (ModulePresetIds::file.toString())
        && xml.getStringAttribute (ModulePresetIds::module.toString()) == moduleName
        && xml.getIntAttribute (ModulePresetIds::version.toString(), 1) <= formatVersion;
}

void ModulePresetLibrary::buildFactoryList (const char* const* list, int size)
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
        if (xml == nullptr || ! accepts (*xml))
            continue;   // a global preset, another module's, or not a preset at all

        auto name = xml->getStringAttribute (ModulePresetIds::name.toString());
        if (name.isEmpty())
            name = res.dropLastCharacters (4).replace ("_", " ");

        factoryPresets.push_back ({ name, true, {}, res });
    }
}

void ModulePresetLibrary::rescan()
{
    std::vector<Preset> found;

    auto files = userDir.findChildFiles (juce::File::findFiles, false, "*.xml");
    files.sort();

    for (const auto& f : files)
    {
        auto xml = juce::XmlDocument::parse (f);
        if (xml == nullptr || ! accepts (*xml))
            continue;

        auto name = xml->getStringAttribute (ModulePresetIds::name.toString());
        if (name.isEmpty())
            name = f.getFileNameWithoutExtension();

        found.push_back ({ name, false, f, {} });
    }

    const auto same = [] (const std::vector<Preset>& a, const std::vector<Preset>& b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].name != b[i].name || a[i].file != b[i].file)
                return false;
        return true;
    };

    if (same (found, userPresets))
        return;

    userPresets = std::move (found);
    sendChangeMessage();
}

std::unique_ptr<juce::XmlElement> ModulePresetLibrary::read (const Preset& preset) const
{
    std::unique_ptr<juce::XmlElement> xml;

    if (preset.isFactory)
    {
        if (getResource == nullptr)
            return {};
        int dataSize = 0;
        if (const char* data = getResource (preset.resourceName.toRawUTF8(), dataSize))
            xml = juce::XmlDocument::parse (juce::String::fromUTF8 (data, dataSize));
    }
    else
    {
        xml = juce::XmlDocument::parse (preset.file);
    }

    if (xml == nullptr || ! accepts (*xml))
        return {};

    return xml;
}

//==============================================================================
juce::File ModulePresetLibrary::fileFor (const juce::String& name) const
{
    return userDir.getChildFile (juce::File::createLegalFileName (name)).withFileExtension ("xml");
}

bool ModulePresetLibrary::write (const juce::String& rawName, const juce::XmlElement& preset)
{
    const auto name = rawName.trim();
    if (name.isEmpty() || ! userDir.createDirectory().wasOk())
        return false;

    juce::XmlElement xml (preset);
    xml.setTagName (ModulePresetIds::file.toString());
    xml.setAttribute (ModulePresetIds::module.toString(), moduleName);
    xml.setAttribute (ModulePresetIds::version.toString(), formatVersion);
    xml.setAttribute (ModulePresetIds::name.toString(), name);

    if (! xml.writeTo (fileFor (name)))
        return false;

    rescan();
    return true;
}

bool ModulePresetLibrary::remove (const Preset& preset)
{
    if (preset.isFactory || ! preset.file.existsAsFile())
        return false;

    if (! preset.file.moveToTrash() && ! preset.file.deleteFile())
        return false;

    rescan();
    return true;
}

bool ModulePresetLibrary::rename (const Preset& preset, const juce::String& rawNewName)
{
    const auto newName = rawNewName.trim();
    if (preset.isFactory || newName.isEmpty() || ! preset.file.existsAsFile())
        return false;

    auto xml = juce::XmlDocument::parse (preset.file);
    if (xml == nullptr || ! accepts (*xml))
        return false;

    xml->setAttribute (ModulePresetIds::name.toString(), newName);

    const auto newFile = fileFor (newName);
    if (! xml->writeTo (newFile))
        return false;
    if (newFile != preset.file)
        preset.file.deleteFile();

    listeners.call ([&] (Listener& l) { l.modulePresetRenamed (preset.name, newName); });
    rescan();
    return true;
}

} // namespace fxme
