/*
  ==============================================================================

    EmbeddedData.h

    Embeds any file (a neural amp model, a table, a text) inside a plugin's
    state ValueTree, so that presets and host sessions are self-contained:
    the counterpart of fxme::EmbeddedAudio and fxme::EmbeddedImage for bytes
    that are neither audio nor a picture. The bytes come back exactly.

    Storage (version 1): the bytes deflated (zlib), then Base64 (7-bit ASCII:
    survives XML on any platform), with the original name and size. In a
    state, under one "EmbeddedData" child:

        <Parameters ...>
          <EmbeddedData>
            <Data slot="Main_NAmp_Model" version="1" name="Plexi.nam"
                  size="412345" data="...base64 of the deflated bytes..."/>
          </EmbeddedData>
        </Parameters>

    The slot is any string unique to the consumer (typically the parameter
    prefix of the effect instance and a name). Since PresetManager and
    get/setStateInformation round-trip the whole state tree, the data
    travels through presets and sessions.

    The same entry also stands alone (writeEntry / readEntry), for an element
    the consumer places itself: a module preset's <Extra>, for instance.

    Size: JSON deflates well (a NAM model of 400 kB of text is about 150 kB
    deflated, 200 kB in Base64). getStoredSize() tells the cost.

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

namespace fxme
{

class EmbeddedData
{
public:
    EmbeddedData() = delete;   // static-only utility

    // In a state tree (call from the message thread when the tree is a live
    // APVTS state).

    /** Stores `size` bytes under state/EmbeddedData at `slotId`, replacing
        any previous entry. False if they could not be compressed. */
    static bool embed (juce::ValueTree state, const juce::String& slotId, const juce::String& name,
                       const void* data, size_t size);

    /** Reads `file` and embeds it under its file name. False if unreadable. */
    static bool embedFile (juce::ValueTree state, const juce::String& slotId, const juce::File& file);

    /** The bytes at `slotId`, empty when absent, corrupt, or written by a
        newer format version than this build knows. */
    static juce::MemoryBlock read (const juce::ValueTree& state, const juce::String& slotId);

    static bool hasEmbedded (const juce::ValueTree& state, const juce::String& slotId);
    static juce::String getEmbeddedName (const juce::ValueTree& state, const juce::String& slotId);
    /** The Base64 text's length: what the entry adds to a saved state. */
    static int getStoredSize (const juce::ValueTree& state, const juce::String& slotId);
    static void removeEmbedded (juce::ValueTree state, const juce::String& slotId);

    // A standalone entry: the same attributes on an element of the caller's.

    /** Sets name, version, size and data on `element`. */
    static bool writeEntry (juce::XmlElement& element, const juce::String& name, const void* data, size_t size);
    /** The bytes of an element written by writeEntry (`name` set when not
        null); empty on failure. */
    static juce::MemoryBlock readEntry (const juce::XmlElement& element, juce::String* name = nullptr);

    static const juce::Identifier containerType;   // "EmbeddedData"
    static const juce::Identifier entryType;       // "Data"
    static const juce::Identifier slotProperty;    // "slot"
    static const juce::Identifier nameProperty;    // "name"
    static const juce::Identifier sizeProperty;    // "size": the bytes before compression
    static const juce::Identifier dataProperty;    // "data": Base64 of the deflated bytes
    static const juce::Identifier versionProperty; // "version"

    static constexpr int currentFormatVersion = 1;

    /** The encoding itself: deflate + Base64, and back (empty on failure;
        `expectedSize` 0: no check). */
    static juce::String encode (const void* data, size_t size);
    static juce::MemoryBlock decode (const juce::String& base64, size_t expectedSize = 0);

private:
    static juce::ValueTree findEntry (const juce::ValueTree& state, const juce::String& slotId);
};

} // namespace fxme
