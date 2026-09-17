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

const juce::Identifier EmbeddedAudio::containerType   ("EmbeddedAudio");
const juce::Identifier EmbeddedAudio::entryType       ("Audio");
const juce::Identifier EmbeddedAudio::slotProperty    ("slot");
const juce::Identifier EmbeddedAudio::nameProperty    ("name");
const juce::Identifier EmbeddedAudio::dataProperty    ("data");
const juce::Identifier EmbeddedAudio::versionProperty ("version");

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
        || reader->numChannels < 1 || reader->numChannels > 8)   // sanity, not a format limit
        return false;

    // 32-bit float WAV in memory. Impulse responses legitimately peak above
    // digital full scale (a minimum-phase correction filter concentrates its
    // energy in its first samples and routinely does), and no integer format
    // can hold that: the earlier FLAC storage clamped those samples and handed
    // back a different filter. Float32 round-trips them exactly, and is also
    // exact for 16- and 24-bit sources, whose values all fit a float mantissa.
    juce::MemoryBlock wavBytes;
    {
        // createWriterFor() binds the stream by reference and moves ownership
        // out only on success, hence unique_ptr<OutputStream> rather than to
        // the concrete type.
        std::unique_ptr<juce::OutputStream> stream
            = std::make_unique<juce::MemoryOutputStream> (wavBytes, false);

        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor (stream,
                                           juce::AudioFormatWriterOptions{}
                                               .withSampleRate    (reader->sampleRate)
                                               .withNumChannels   ((int) reader->numChannels)
                                               .withBitsPerSample (32));

        if (writer == nullptr || ! writer->writeFromAudioReader (*reader, 0, reader->lengthInSamples))
            return false;
        // The writer destructor finalises the WAV header.
    }

    // Deflate. Float PCM compresses poorly (a few percent on a typical
    // impulse response, against roughly 60% for FLAC on the same data), which
    // is the price of a container that can represent the samples at all.
    juce::MemoryBlock packed;
    {
        juce::MemoryOutputStream packedStream (packed, false);
        {
            juce::GZIPCompressorOutputStream deflate (packedStream, 9);
            if (! deflate.write (wavBytes.getData(), wavBytes.getSize()))
                return false;
        }   // deflate's destructor finishes the zlib stream
    }       // packedStream's destructor trims `packed` to what was written

    auto container = state.getOrCreateChildWithName (containerType, nullptr);
    auto entry = findEntry (state, slotId);
    if (! entry.isValid())
    {
        entry = juce::ValueTree (entryType);
        entry.setProperty (slotProperty, slotId, nullptr);
        container.appendChild (entry, nullptr);
    }

    entry.setProperty (versionProperty, currentFormatVersion, nullptr);
    entry.setProperty (nameProperty, sourceFile.getFileName(), nullptr);
    entry.setProperty (dataProperty,
                       juce::Base64::toBase64 (packed.getData(), packed.getSize()),
                       nullptr);
    return true;
}

std::unique_ptr<juce::AudioFormatReader> EmbeddedAudio::createReader (const juce::ValueTree& state,
                                                                      const juce::String& slotId)
{
    const auto entry = findEntry (state, slotId);
    if (! entry.isValid())
        return nullptr;

    const int version = (int) entry[versionProperty];
    if (version > currentFormatVersion)
        return nullptr;             // written by a newer build than this one

    juce::MemoryOutputStream decoded;
    if (! juce::Base64::convertFromBase64 (decoded, entry[dataProperty].toString())
        || decoded.getDataSize() == 0)
        return nullptr;

    // Version 0 is the original format, raw FLAC with no version attribute at
    // all. Presets and sessions of every plugin that shipped before the change
    // hold it, including factory presets compiled into binary data, so it stays
    // readable. Its over-full-scale samples were clamped when they were stored
    // and cannot be recovered here; re-embedding the source file repairs that.
    if (version == 0)
    {
        juce::FlacAudioFormat flac;
        return std::unique_ptr<juce::AudioFormatReader> (
            flac.createReaderFor (new juce::MemoryInputStream (decoded.getMemoryBlock(), true),
                                  true));
    }

    juce::MemoryBlock wavBytes;
    {
        juce::MemoryInputStream packed (decoded.getMemoryBlock(), false);
        juce::GZIPDecompressorInputStream inflate (&packed, false);
        juce::MemoryOutputStream out (wavBytes, false);
        if (out.writeFromInputStream (inflate, -1) <= 0)
            return nullptr;
    }   // out's destructor trims wavBytes to what was inflated

    juce::WavAudioFormat wav;
    return std::unique_ptr<juce::AudioFormatReader> (
        wav.createReaderFor (new juce::MemoryInputStream (wavBytes, true), true));
}

bool EmbeddedAudio::hasEmbedded (const juce::ValueTree& state, const juce::String& slotId)
{
    const auto entry = findEntry (state, slotId);
    return entry.isValid()
        && (int) entry[versionProperty] <= currentFormatVersion
        && entry[dataProperty].toString().isNotEmpty();
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
