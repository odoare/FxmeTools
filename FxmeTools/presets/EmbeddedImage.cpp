/*
  ==============================================================================

    EmbeddedImage.cpp

  ==============================================================================
*/

#include "EmbeddedImage.h"

namespace fxme
{

const juce::Identifier EmbeddedImage::containerType  ("EmbeddedImages");
const juce::Identifier EmbeddedImage::entryType      ("Image");
const juce::Identifier EmbeddedImage::slotProperty   ("slot");
const juce::Identifier EmbeddedImage::nameProperty   ("name");
const juce::Identifier EmbeddedImage::widthProperty  ("w");
const juce::Identifier EmbeddedImage::heightProperty ("h");
const juce::Identifier EmbeddedImage::dataProperty   ("data");

juce::ValueTree EmbeddedImage::findEntry (const juce::ValueTree& state, const juce::String& slotId)
{
    auto container = state.getChildWithName (containerType);
    if (container.isValid())
        for (const auto& child : container)
            if (child.hasType (entryType) && child[slotProperty].toString() == slotId)
                return child;
    return {};
}

bool EmbeddedImage::embed (juce::ValueTree state,
                           const juce::String& slotId,
                           const juce::Image& image,
                           const juce::String& name,
                           const Options& options)
{
    if (! image.isValid() || image.getWidth() < 1 || image.getHeight() < 1)
        return false;

    // Downscale before encoding: presets live inside session files, and the
    // consumer rarely needs more than ~1 MP.
    juce::Image toEncode = image;
    const int longest = juce::jmax (image.getWidth(), image.getHeight());

    if (options.maxDimension > 0 && longest > options.maxDimension)
    {
        const float scale = (float) options.maxDimension / (float) longest;
        toEncode = image.rescaled (juce::jmax (1, juce::roundToInt ((float) image.getWidth() * scale)),
                                   juce::jmax (1, juce::roundToInt ((float) image.getHeight() * scale)),
                                   juce::Graphics::highResamplingQuality);
    }

    juce::MemoryBlock encoded;
    {
        juce::MemoryOutputStream stream (encoded, false);

        if (options.format == Format::png)
        {
            juce::PNGImageFormat png;
            if (! png.writeImageToStream (toEncode, stream))
                return false;
        }
        else
        {
            juce::JPEGImageFormat jpeg;
            jpeg.setQuality (juce::jlimit (0.0f, 1.0f, options.jpegQuality));
            if (! jpeg.writeImageToStream (toEncode, stream))
                return false;
        }
    }

    if (encoded.getSize() == 0)
        return false;

    auto container = state.getOrCreateChildWithName (containerType, nullptr);
    auto entry = findEntry (state, slotId);
    if (! entry.isValid())
    {
        entry = juce::ValueTree (entryType);
        entry.setProperty (slotProperty, slotId, nullptr);
        container.appendChild (entry, nullptr);
    }

    entry.setProperty (nameProperty, name, nullptr);
    entry.setProperty (widthProperty, toEncode.getWidth(), nullptr);
    entry.setProperty (heightProperty, toEncode.getHeight(), nullptr);
    entry.setProperty (dataProperty,
                       juce::Base64::toBase64 (encoded.getData(), encoded.getSize()),
                       nullptr);
    return true;
}

bool EmbeddedImage::embedFile (juce::ValueTree state,
                               const juce::String& slotId,
                               const juce::File& sourceFile,
                               const Options& options)
{
    const auto image = juce::ImageFileFormat::loadFrom (sourceFile);
    if (! image.isValid())
        return false;

    return embed (state, slotId, image, sourceFile.getFileName(), options);
}

juce::Image EmbeddedImage::load (const juce::ValueTree& state, const juce::String& slotId)
{
    auto entry = findEntry (state, slotId);
    if (! entry.isValid())
        return {};

    const auto base64 = entry[dataProperty].toString();
    if (base64.isEmpty())
        return {};

    juce::MemoryBlock bytes;
    {
        juce::MemoryOutputStream stream (bytes, false);
        if (! juce::Base64::convertFromBase64 (stream, base64))
            return {};
    }

    return juce::ImageFileFormat::loadFrom (bytes.getData(), bytes.getSize());
}

bool EmbeddedImage::hasEmbedded (const juce::ValueTree& state, const juce::String& slotId)
{
    auto entry = findEntry (state, slotId);
    return entry.isValid() && entry[dataProperty].toString().isNotEmpty();
}

juce::String EmbeddedImage::getEmbeddedName (const juce::ValueTree& state, const juce::String& slotId)
{
    auto entry = findEntry (state, slotId);
    return entry.isValid() ? entry[nameProperty].toString() : juce::String();
}

juce::Point<int> EmbeddedImage::getEmbeddedDimensions (const juce::ValueTree& state, const juce::String& slotId)
{
    auto entry = findEntry (state, slotId);
    if (! entry.isValid())
        return {};

    return { (int) entry[widthProperty], (int) entry[heightProperty] };
}

int EmbeddedImage::getEmbeddedSizeBytes (const juce::ValueTree& state, const juce::String& slotId)
{
    auto entry = findEntry (state, slotId);
    return entry.isValid() ? entry[dataProperty].toString().length() : 0;
}

void EmbeddedImage::removeEmbedded (juce::ValueTree state, const juce::String& slotId)
{
    auto container = state.getChildWithName (containerType);
    if (! container.isValid())
        return;

    auto entry = findEntry (state, slotId);
    if (entry.isValid())
        container.removeChild (entry, nullptr);

    if (container.getNumChildren() == 0)
        state.removeChild (container, nullptr);
}

} // namespace fxme
