/*
  ==============================================================================

    EmbeddedAudio.h

    Embeds audio files (impulse responses, samples…) inside a plugin's state
    ValueTree so that presets and host sessions are fully self-contained,
    instead of referencing a file path that may not exist on another machine.

    Storage format (version 1): the source audio is re-encoded to a 32-bit
    float WAV in memory, deflated (zlib), then Base64-encoded. Base64 is pure
    7-bit ASCII, so the result survives XML serialisation on any platform.
    Entries live under a single "EmbeddedAudio" child of the state:

        <Parameters ...>
          <EmbeddedAudio>
            <Audio slot="bus_fx3_Rev_ExtIR" version="1" name="MyHall.wav"
                   data="...base64 of deflated float WAV..."/>
          </EmbeddedAudio>
        </Parameters>

    The slot id is any string unique to the consumer (typically the parameter
    prefix of the effect instance). Because PresetManager and
    get/setStateInformation round-trip the whole state tree, embedded audio
    automatically travels through presets and DAW sessions.

    Why float and not FLAC. An impulse response legitimately peaks above
    digital full scale — a minimum-phase correction filter concentrates its
    energy in its first samples and routinely exceeds it by several dB — and no
    integer format can represent that. FLAC storage (used before version 1)
    clamped those samples, so a state round trip silently returned a different
    filter: measured in a room, a minimum-phase correction came back about 4 dB
    low with 2 dB of ripple. Float32 round-trips exactly, and is also exact for
    16- and 24-bit sources, whose values all fit a float mantissa. It costs
    size, since float PCM deflates by only a few percent where FLAC reached
    roughly 60%, which is the price of a container that can hold the data.

    Each entry carries the format version it was written with. Version 1 is the
    format above; version 0 is the original one, raw FLAC with no version
    attribute at all, which createReader still reads so that presets and
    sessions written by any plugin before the change keep working (factory
    presets compiled into binary data included). Only version 1 is written.
    Clamping already applied to a version 0 payload cannot be undone on the way
    back in; re-embedding the source file is what repairs it. A payload from a
    newer version than this build knows is refused rather than guessed at.

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

class EmbeddedAudio
{
public:
    EmbeddedAudio() = delete;   // static-only utility

    // Reads sourceFile (wav/aiff/flac/ogg), re-encodes it as a deflated
    // 32-bit float WAV, Base64s it and stores it under state/EmbeddedAudio at
    // the given slot (replacing any previous entry), stamped with
    // currentFormatVersion. Returns false if the file can't be read or
    // encoded. Call from the message thread.
    static bool embedFile (juce::ValueTree state,
                           const juce::String& slotId,
                           const juce::File& sourceFile);

    // Creates a reader for the audio embedded at slotId, or nullptr if the
    // slot is absent, corrupt, or written by a newer format version than this
    // build knows. Reads every version up to currentFormatVersion. The reader owns a copy of the decoded bytes, so it stays valid
    // even if the state entry is replaced afterwards.
    static std::unique_ptr<juce::AudioFormatReader> createReader (const juce::ValueTree& state,
                                                                  const juce::String& slotId);

    static bool hasEmbedded (const juce::ValueTree& state, const juce::String& slotId);

    // Original file name of the embedded audio (e.g. "MyHall.wav"), or empty.
    static juce::String getEmbeddedName (const juce::ValueTree& state, const juce::String& slotId);

    static void removeEmbedded (juce::ValueTree state, const juce::String& slotId);

    static const juce::Identifier containerType;   // "EmbeddedAudio"
    static const juce::Identifier entryType;       // "Audio"
    static const juce::Identifier slotProperty;    // "slot"
    static const juce::Identifier nameProperty;    // "name"
    static const juce::Identifier dataProperty;    // "data" — Base64 of the deflated WAV
    static const juce::Identifier versionProperty; // "version" — storage format,
                                                   //   absent means 0 (FLAC)

    // Bump when the stored representation changes, and handle the older value
    // in createReader.
    static constexpr int currentFormatVersion = 1;

private:
    static juce::ValueTree findEntry (const juce::ValueTree& state, const juce::String& slotId);
};

} // namespace fxme
