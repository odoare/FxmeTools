/*
  ------------------------------------------------------------------------------
    StillImageSource.h

    FrameSource for a fixed picture: a file on disk or an image already in
    memory (a generated pattern, a BinaryData asset, ...). It emits the image
    once on start(); the VideoEngine keeps applying adjustments to it, so a
    still source costs nothing in the steady state.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include "FrameSource.h"

namespace fxme
{

/**
 * @class StillImageSource
 * @brief One-shot FrameSource wrapping a file or an in-memory juce::Image.
 */
class StillImageSource : public FrameSource
{
public:
    /** Loads the picture with juce::ImageFileFormat; start() fails if the file
        is not a readable image. */
    explicit StillImageSource (const juce::File& fileToLoad)
        : file (fileToLoad), image (juce::ImageFileFormat::loadFrom (fileToLoad))
    {
    }

    /** Uses an image already in memory. `displayName` is what getName() reports. */
    StillImageSource (const juce::Image& imageToUse, const juce::String& displayName)
        : name (displayName), image (imageToUse)
    {
    }

    bool start() override
    {
        if (! image.isValid())
            return false;

        if (onFrame != nullptr)
            onFrame (image);

        return true;
    }

    void stop() override {}

    juce::String getName() const override { return name.isNotEmpty() ? name : file.getFileName(); }
    bool isLive() const override          { return false; }

    juce::File getFile() const            { return file; }
    juce::Image getImage() const          { return image; }

private:
    juce::File file;
    juce::String name;
    juce::Image image;
};

} // namespace fxme
