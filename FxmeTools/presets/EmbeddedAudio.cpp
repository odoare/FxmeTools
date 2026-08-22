/*
  ==============================================================================

    EmbeddedAudio.cpp

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ==============================================================================
*/

#include "EmbeddedAudio.h"

namespace fxme
{

const juce::Identifier EmbeddedAudio::containerType ("EmbeddedAudio");
const juce::Identifier EmbeddedAudio::entryType     ("Audio");
const juce::Identifier EmbeddedAudio::slotProperty  ("slot");
const juce::Identifier EmbeddedAudio::nameProperty  ("name");
const juce::Identifier EmbeddedAudio::dataProperty  ("data");

juce::ValueTree EmbeddedAudio::findEntry (const juce::ValueTree& state, const juce::String& slotId)
{
    auto container = state.getChildWithName (containerType);
    if (container.isValid())
        for (const auto& child : container)
            if (child.hasType (entryType) && child[slotProperty].toString() == slotId)
                return child;
    return {};
}

bool EmbeddedAudio::embedFile (juce::ValueTree state,
                               const juce::String& slotId,
                               const juce::File& sourceFile)
{
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (sourceFile));

    if (reader == nullptr || reader->lengthInSamples <= 0
        || reader->numChannels < 1 || reader->numChannels > 8)   // FLAC channel limit
        return false;

    const int bits = (reader->bitsPerSample > 16 || reader->usesFloatingPointData) ? 24 : 16;

    juce::MemoryBlock flacBytes;
    {
        // On success the writer takes ownership of the stream and the bytes
        // live on in flacBytes; on failure the stream stays with `stream` here
        // and is released at the end of this scope. It must be declared as a
        // unique_ptr<OutputStream> rather than to the concrete type, because
        // createWriterFor() binds it by reference to move ownership out.
        std::unique_ptr<juce::OutputStream> stream
            = std::make_unique<juce::MemoryOutputStream> (flacBytes, false);

        juce::FlacAudioFormat flac;
        auto writer = flac.createWriterFor (stream,
                                            juce::AudioFormatWriterOptions{}
                                                .withSampleRate        (reader->sampleRate)
                                                .withNumChannels       ((int) reader->numChannels)
                                                .withBitsPerSample     (bits)
                                                .withQualityOptionIndex (5));

        if (writer == nullptr || ! writer->writeFromAudioReader (*reader, 0, reader->lengthInSamples))
            return false;
        // The writer destructor finalises the FLAC stream.
    }

    auto container = state.getOrCreateChildWithName (containerType, nullptr);
    auto entry = findEntry (state, slotId);
    if (! entry.isValid())
    {
        entry = juce::ValueTree (entryType);
        entry.setProperty (slotProperty, slotId, nullptr);
        container.appendChild (entry, nullptr);
    }

    entry.setProperty (nameProperty, sourceFile.getFileName(), nullptr);
    entry.setProperty (dataProperty,
                       juce::Base64::toBase64 (flacBytes.getData(), flacBytes.getSize()),
                       nullptr);
    return true;
}

std::unique_ptr<juce::AudioFormatReader> EmbeddedAudio::createReader (const juce::ValueTree& state,
                                                                      const juce::String& slotId)
{
    const auto entry = findEntry (state, slotId);
    if (! entry.isValid())
        return nullptr;

    juce::MemoryOutputStream decoded;
    if (! juce::Base64::convertFromBase64 (decoded, entry[dataProperty].toString())
        || decoded.getDataSize() == 0)
        return nullptr;

    juce::FlacAudioFormat flac;
    return std::unique_ptr<juce::AudioFormatReader> (
        flac.createReaderFor (new juce::MemoryInputStream (decoded.getMemoryBlock(), true), true));
}

bool EmbeddedAudio::hasEmbedded (const juce::ValueTree& state, const juce::String& slotId)
{
    const auto entry = findEntry (state, slotId);
    return entry.isValid() && entry[dataProperty].toString().isNotEmpty();
}

juce::String EmbeddedAudio::getEmbeddedName (const juce::ValueTree& state, const juce::String& slotId)
{
    return findEntry (state, slotId)[nameProperty].toString();
}

void EmbeddedAudio::removeEmbedded (juce::ValueTree state, const juce::String& slotId)
{
    auto entry = findEntry (state, slotId);
    if (entry.isValid())
        entry.getParent().removeChild (entry, nullptr);
}

} // namespace fxme
