/*
  ==============================================================================

    EmbeddedImage.h

    Embeds pictures inside a plugin's state ValueTree so that presets and host
    sessions are fully self-contained, instead of referencing a file path that
    may not exist on another machine. The image counterpart of
    fxme::EmbeddedAudio, with the same storage conventions.

    Storage format: the image is optionally downscaled, encoded to PNG or
    JPEG in memory, then Base64-encoded. Base64 is pure 7-bit ASCII, so the
    result survives XML serialisation on any platform. Entries live under a
    single "EmbeddedImages" child of the state:

        <Parameters ...>
          <EmbeddedImages>
            <Image slot="terrain" name="clouds.jpg" w="1024" h="768"
                   data="...base64 JPEG..."/>
          </EmbeddedImages>
        </Parameters>

    The slot id is any string unique to the consumer. Because PresetManager
    and get/setStateInformation round-trip the whole state tree, embedded
    images automatically travel through presets and DAW sessions.

    Size matters here: a preset is XML in a session file, so the default
    Options downscale to 1024 px and encode as JPEG (typically 100–250 kB of
    Base64). Use Format::png when the picture must stay lossless (generated
    patterns, sharp graphics), and getEmbeddedSizeBytes() to check the cost.

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

/** Container format for an embedded picture. */
enum class ImageEncoding { jpeg, png };

/** How a picture is re-encoded before it goes into the state.
    (At namespace scope so its defaults can be used as default arguments;
    also available as fxme::EmbeddedImage::Options.) */
struct EmbeddedImageOptions
{
    int maxDimension = 1024;                          ///< downscale longest side before encoding (0 = keep)
    ImageEncoding format = ImageEncoding::jpeg;       ///< jpeg: small, photographic; png: lossless
    float jpegQuality = 0.9f;                         ///< 0..1, ignored for png
};

class EmbeddedImage
{
public:
    EmbeddedImage() = delete;   // static-only utility

    using Format = ImageEncoding;
    using Options = EmbeddedImageOptions;

    /** Encodes `image` and stores it under state/EmbeddedImages at the given
        slot (replacing any previous entry). `name` is a free-text label kept
        for the UI (usually the original file name). Returns false if the
        image is invalid or could not be encoded. Message thread. */
    static bool embed (juce::ValueTree state,
                       const juce::String& slotId,
                       const juce::Image& image,
                       const juce::String& name = juce::String(),
                       const Options& options = {});

    /** Loads an image file and embeds it (same re-encoding as embed()), using
        the file name as the label. Returns false if the file is not a
        readable image. */
    static bool embedFile (juce::ValueTree state,
                           const juce::String& slotId,
                           const juce::File& sourceFile,
                           const Options& options = {});

    /** Decodes the image embedded at slotId, or an invalid juce::Image if the
        slot is absent or corrupt. */
    static juce::Image load (const juce::ValueTree& state, const juce::String& slotId);

    static bool hasEmbedded (const juce::ValueTree& state, const juce::String& slotId);

    /** Label stored with the image (e.g. "clouds.jpg"), or empty. */
    static juce::String getEmbeddedName (const juce::ValueTree& state, const juce::String& slotId);

    /** Pixel size of the embedded image without decoding it (0×0 if absent). */
    static juce::Point<int> getEmbeddedDimensions (const juce::ValueTree& state, const juce::String& slotId);

    /** Approximate cost of the entry in the serialised state, in bytes. */
    static int getEmbeddedSizeBytes (const juce::ValueTree& state, const juce::String& slotId);

    static void removeEmbedded (juce::ValueTree state, const juce::String& slotId);

    static const juce::Identifier containerType;   // "EmbeddedImages"
    static const juce::Identifier entryType;       // "Image"
    static const juce::Identifier slotProperty;    // "slot"
    static const juce::Identifier nameProperty;    // "name"
    static const juce::Identifier widthProperty;   // "w"
    static const juce::Identifier heightProperty;  // "h"
    static const juce::Identifier dataProperty;    // "data" — Base64 of PNG/JPEG bytes

private:
    static juce::ValueTree findEntry (const juce::ValueTree& state, const juce::String& slotId);
};

} // namespace fxme
