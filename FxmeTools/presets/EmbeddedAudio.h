/*
  ==============================================================================

    EmbeddedAudio.h

    Embeds audio files (impulse responses, samples…) inside a plugin's state
    ValueTree so that presets and host sessions are fully self-contained,
    instead of referencing a file path that may not exist on another machine.

    Storage format: the source audio is re-encoded losslessly to FLAC in
    memory (typically 40-60% of the WAV size), then Base64-encoded. Base64 is
    pure 7-bit ASCII, so the result survives XML serialisation on any
    platform. Entries live under a single "EmbeddedAudio" child of the state:

        <Parameters ...>
          <EmbeddedAudio>
            <Audio slot="bus_fx3_Rev_ExtIR" name="MyHall.wav" data="...base64 FLAC..."/>
          </EmbeddedAudio>
        </Parameters>

    The slot id is any string unique to the consumer (typically the parameter
    prefix of the effect instance). Because PresetManager and
    get/setStateInformation round-trip the whole state tree, embedded audio
    automatically travels through presets and DAW sessions.

    FLAC is integer-only: sources are stored at 24-bit (16-bit sources at
    16-bit), which is bit-transparent for integer sources and ~144 dB of
    dynamic range for float ones — inaudible for impulse responses.

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

    // Reads sourceFile (wav/aiff/flac/ogg), FLAC+Base64 encodes it and stores
    // it under state/EmbeddedAudio at the given slot (replacing any previous
    // entry). Returns false if the file can't be read or encoded.
    // Call from the message thread.
    static bool embedFile (juce::ValueTree state,
                           const juce::String& slotId,
                           const juce::File& sourceFile);

    // Creates a reader for the audio embedded at slotId, or nullptr if the
    // slot is absent/corrupt. The reader owns a copy of the decoded bytes, so
    // it stays valid even if the state entry is replaced afterwards.
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
    static const juce::Identifier dataProperty;    // "data" — Base64 of FLAC bytes

private:
    static juce::ValueTree findEntry (const juce::ValueTree& state, const juce::String& slotId);
};

} // namespace fxme
