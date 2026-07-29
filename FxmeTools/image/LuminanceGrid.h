/*
  ------------------------------------------------------------------------------
    LuminanceGrid.h

    Turns a juce::Image into a small grid of luminance values in [0, 1] and
    keeps the previous grid, so image analysis (motion/optical flow, region
    statistics, colour tracking) works on plain floats instead of pixels.

    Typical use (message thread, once per captured frame):

        fxme::LuminanceGrid grid { 160 };          // max width in cells
        grid.update (processedFrame);
        if (grid.hasPrevious())
            for (int y = 1; y < grid.getHeight() - 1; ++y)
                for (int x = 1; x < grid.getWidth() - 1; ++x)
                    auto dt = grid.at (x, y) - grid.previousAt (x, y);

    update() reuses its buffers, so the steady state performs no allocation.
    Resizing (a new capture resolution or a new cell width) drops the history
    and hasPrevious() returns false for one frame.

    Header-only, depends only on juce_graphics.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_graphics/juce_graphics.h>
#include <vector>
#include <algorithm>

namespace fxme
{

/**
 * @class LuminanceGrid
 * @brief Downsampled luminance field of an image, with one frame of history.
 */
class LuminanceGrid
{
public:
    /** How a pixel is reduced to one luminance value. */
    enum class Mode
    {
        maxChannel,   ///< max(R, G, B) — bright colours read as bright (default)
        rec601        ///< 0.299 R + 0.587 G + 0.114 B — perceptual luma
    };

    /** @param maxWidthCells  longest horizontal size of the grid (height follows
                              the image aspect ratio). */
    explicit LuminanceGrid (int maxWidthCells = 160, Mode modeToUse = Mode::maxChannel)
        : maxWidth (juce::jmax (2, maxWidthCells)), mode (modeToUse) {}

    void setMaxWidth (int newMaxWidthCells)
    {
        const int w = juce::jmax (2, newMaxWidthCells);
        if (w != maxWidth)
        {
            maxWidth = w;
            width = height = 0;   // forces a resize (and drops the history)
        }
    }

    void setMode (Mode newMode) noexcept   { mode = newMode; }

    /** Samples `image` into the grid; the previous contents become the
        history (see previousAt / hasPrevious). Ignores invalid images. */
    void update (const juce::Image& image)
    {
        if (! image.isValid() || image.getWidth() < 2 || image.getHeight() < 2)
            return;

        const int w = juce::jmin (maxWidth, image.getWidth());
        const int h = juce::jmax (2, image.getHeight() * w / image.getWidth());

        if (w != width || h != height)
        {
            width = w;
            height = h;
            current.assign ((size_t) w * (size_t) h, 0.0f);
            previous.clear();
        }
        else
        {
            previous.swap (current);
            current.resize ((size_t) w * (size_t) h);
        }

        const juce::Image::BitmapData data (image, juce::Image::BitmapData::readOnly);
        const bool useMax = (mode == Mode::maxChannel);

        for (int gy = 0; gy < h; ++gy)
        {
            const int sy = gy * (data.height - 1) / (h - 1);
            const juce::uint8* row = data.getLinePointer (sy);
            float* out = current.data() + (size_t) gy * (size_t) w;

            for (int gx = 0; gx < w; ++gx)
            {
                const int sx = gx * (data.width - 1) / (w - 1);
                const juce::uint8* p = row + (size_t) sx * (size_t) data.pixelStride;

                out[gx] = useMax ? (float) juce::jmax (p[0], p[1], p[2]) / 255.0f
                                 : (0.114f * (float) p[0] + 0.587f * (float) p[1]
                                      + 0.299f * (float) p[2]) / 255.0f;
            }
        }
    }

    /** Forgets the history (next update() reports hasPrevious() == false). */
    void reset()                                { previous.clear(); }

    int getWidth() const noexcept               { return width; }
    int getHeight() const noexcept              { return height; }
    bool isValid() const noexcept               { return width > 1 && height > 1; }

    /** True when a same-sized previous grid exists (temporal work is possible). */
    bool hasPrevious() const noexcept           { return previous.size() == current.size() && ! previous.empty(); }

    float at (int x, int y) const noexcept         { return current[(size_t) y * (size_t) width + (size_t) x]; }
    float previousAt (int x, int y) const noexcept { return previous[(size_t) y * (size_t) width + (size_t) x]; }

    /** Raw row-major buffers; previousData() is null unless hasPrevious(). */
    const float* data() const noexcept          { return current.data(); }
    const float* previousData() const noexcept  { return hasPrevious() ? previous.data() : nullptr; }

    /** Grid cell centre in normalised image coordinates ([0,1]²). */
    juce::Point<float> normalisedCentre (int x, int y) const noexcept
    {
        return { ((float) x + 0.5f) / (float) width, ((float) y + 0.5f) / (float) height };
    }

private:
    int maxWidth = 160;
    Mode mode = Mode::maxChannel;
    int width = 0, height = 0;
    std::vector<float> current, previous;
};

} // namespace fxme
