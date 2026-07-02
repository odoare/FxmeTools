/*
  ==============================================================================

    EmbeddedAudio.cpp

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
        // The writer owns and deletes the stream; the bytes live on in flacBytes.
        juce::FlacAudioFormat flac;
        std::unique_ptr<juce::AudioFormatWriter> writer (
            flac.createWriterFor (new juce::MemoryOutputStream (flacBytes, false),
                                  reader->sampleRate, reader->numChannels, bits, {}, 5));

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
