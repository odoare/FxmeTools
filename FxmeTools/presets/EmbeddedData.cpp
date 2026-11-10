/*
  ==============================================================================

    EmbeddedData.cpp

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ==============================================================================
*/

namespace fxme
{

const juce::Identifier EmbeddedData::containerType   { "EmbeddedData" };
const juce::Identifier EmbeddedData::entryType       { "Data" };
const juce::Identifier EmbeddedData::slotProperty    { "slot" };
const juce::Identifier EmbeddedData::nameProperty    { "name" };
const juce::Identifier EmbeddedData::sizeProperty    { "size" };
const juce::Identifier EmbeddedData::dataProperty    { "data" };
const juce::Identifier EmbeddedData::versionProperty { "version" };

juce::String EmbeddedData::encode (const void* data, size_t size)
{
    juce::MemoryBlock packed;
    {
        juce::MemoryOutputStream packedStream (packed, false);
        {
            juce::GZIPCompressorOutputStream deflate (packedStream, 9);
            if (size > 0 && ! deflate.write (data, size))
                return {};
        }   // finishes the zlib stream
    }       // trims `packed` to what was written
    return juce::Base64::toBase64 (packed.getData(), packed.getSize());
}

juce::MemoryBlock EmbeddedData::decode (const juce::String& base64, size_t expectedSize)
{
    juce::MemoryOutputStream decoded;
    if (base64.isEmpty() || ! juce::Base64::convertFromBase64 (decoded, base64) || decoded.getDataSize() == 0)
        return {};

    juce::MemoryBlock bytes;
    {
        // Over the decoded bytes themselves, alive until the end of this block
        // (see EmbeddedAudio::createReader: a stream on a temporary copy
        // inflated garbage).
        juce::MemoryInputStream packed (decoded.getData(), decoded.getDataSize(), false);
        juce::GZIPDecompressorInputStream inflate (&packed, false);
        juce::MemoryOutputStream out (bytes, false);
        out.writeFromInputStream (inflate, -1);
    }
    if (expectedSize != 0 && bytes.getSize() != expectedSize)
        return {};
    return bytes;
}

juce::ValueTree EmbeddedData::findEntry (const juce::ValueTree& state, const juce::String& slotId)
{
    const auto container = state.getChildWithName (containerType);
    if (! container.isValid())
        return {};
    return container.getChildWithProperty (slotProperty, slotId);
}

bool EmbeddedData::embed (juce::ValueTree state, const juce::String& slotId, const juce::String& name,
                          const void* data, size_t size)
{
    const auto text = encode (data, size);
    if (text.isEmpty())
        return false;

    auto container = state.getOrCreateChildWithName (containerType, nullptr);
    auto entry = findEntry (state, slotId);
    if (! entry.isValid())
    {
        entry = juce::ValueTree (entryType);
        entry.setProperty (slotProperty, slotId, nullptr);
        container.appendChild (entry, nullptr);
    }
    entry.setProperty (versionProperty, currentFormatVersion, nullptr);
    entry.setProperty (nameProperty, name, nullptr);
    entry.setProperty (sizeProperty, (juce::int64) size, nullptr);
    entry.setProperty (dataProperty, text, nullptr);
    return true;
}

bool EmbeddedData::embedFile (juce::ValueTree state, const juce::String& slotId, const juce::File& file)
{
    juce::MemoryBlock bytes;
    if (! file.existsAsFile() || ! file.loadFileAsData (bytes) || bytes.getSize() == 0)
        return false;
    return embed (state, slotId, file.getFileName(), bytes.getData(), bytes.getSize());
}

juce::MemoryBlock EmbeddedData::read (const juce::ValueTree& state, const juce::String& slotId)
{
    const auto entry = findEntry (state, slotId);
    if (! entry.isValid() || (int) entry[versionProperty] > currentFormatVersion)
        return {};
    return decode (entry[dataProperty].toString(), (size_t) (juce::int64) entry[sizeProperty]);
}

bool EmbeddedData::hasEmbedded (const juce::ValueTree& state, const juce::String& slotId)
{
    return findEntry (state, slotId).isValid();
}

juce::String EmbeddedData::getEmbeddedName (const juce::ValueTree& state, const juce::String& slotId)
{
    return findEntry (state, slotId)[nameProperty].toString();
}

int EmbeddedData::getStoredSize (const juce::ValueTree& state, const juce::String& slotId)
{
    return findEntry (state, slotId)[dataProperty].toString().length();
}

void EmbeddedData::removeEmbedded (juce::ValueTree state, const juce::String& slotId)
{
    auto container = state.getChildWithName (containerType);
    if (! container.isValid())
        return;
    const auto entry = container.getChildWithProperty (slotProperty, slotId);
    if (entry.isValid())
        container.removeChild (entry, nullptr);
    if (container.getNumChildren() == 0)
        state.removeChild (container, nullptr);
}

bool EmbeddedData::writeEntry (juce::XmlElement& element, const juce::String& name, const void* data, size_t size)
{
    const auto text = encode (data, size);
    if (text.isEmpty())
        return false;
    element.setAttribute (versionProperty, currentFormatVersion);
    element.setAttribute (nameProperty, name);
    element.setAttribute (sizeProperty, juce::String ((juce::int64) size));
    element.setAttribute (dataProperty, text);
    return true;
}

juce::MemoryBlock EmbeddedData::readEntry (const juce::XmlElement& element, juce::String* name)
{
    if (element.getIntAttribute (versionProperty) > currentFormatVersion)
        return {};
    if (name != nullptr)
        *name = element.getStringAttribute (nameProperty);
    return decode (element.getStringAttribute (dataProperty),
                   (size_t) element.getStringAttribute (sizeProperty).getLargeIntValue());
}

} // namespace fxme
