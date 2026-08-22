/*
  ------------------------------------------------------------------------------
    ComponentSnapshot.h

    Renders a juce::Component to a PNG file — for documentation, reports or
    regression screenshots. The component does NOT need to be on screen (or
    have a parent): it is sized, painted into an offscreen image and written
    out, so a display component can double as a figure generator.

        fxme::WaveformDisplay plot;
        plot.setBuffer (ir, sampleRate);
        fxme::saveComponentAsPng (plot, file, 1100, 420);   // 2x by default

    MESSAGE THREAD ONLY: painting a component off the message thread is not
    safe, even when it is unparented.

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>

namespace fxme
{

/** Paints `c` at width x height into an offscreen image and writes it as PNG.
    `scale` multiplies the pixel size (2 keeps text and curves crisp when the
    figure is viewed or printed larger than its logical size). Returns false
    if the size is degenerate, the image could not be rendered, or the file
    could not be written. Any existing file is replaced. */
inline bool saveComponentAsPng (juce::Component& c, const juce::File& file,
                                int width, int height, float scale = 2.0f)
{
    if (width <= 0 || height <= 0 || scale <= 0.0f)
        return false;

    c.setBounds (0, 0, width, height);
    c.setVisible (true);        // freshly built components start hidden
    const auto image = c.createComponentSnapshot (c.getLocalBounds(), false, scale);
    if (! image.isValid())
        return false;

    file.deleteFile();
    std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
    if (stream == nullptr || ! stream->openedOk())
        return false;

    juce::PNGImageFormat png;
    return png.writeImageToStream (image, *stream);
}

} // namespace fxme
