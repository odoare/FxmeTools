/*
  ------------------------------------------------------------------------------
    ImageAdjustments.h

    Brightness / contrast / saturation / mirroring applied in place to an
    ARGB juce::Image, in one pass over the pixels.

    Conventions (chosen so that "all defaults" is a bit-exact no-op, which
    isNeutral() reports so callers can skip the copy entirely):

      brightness  [-1, 1]  additive offset, 0 = neutral
      contrast    [-1, 1]  gain around mid-grey: 1 + contrast, 0 = neutral
      saturation  [ 0, 2]  blend towards Rec.601 luma, 1 = neutral
      mirrorH / mirrorV    flips, false = neutral

    Header-only, depends only on juce_graphics. Message-thread helper: it
    allocates nothing but walks every pixel, so keep it off the audio thread.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_graphics/juce_graphics.h>

namespace fxme
{

/**
 * @struct ImageAdjustments
 * @brief Colour/geometry corrections for a captured frame.
 */
struct ImageAdjustments
{
    float brightness = 0.0f;   ///< [-1, 1], additive
    float contrast   = 0.0f;   ///< [-1, 1], gain = 1 + contrast around 0.5
    float saturation = 1.0f;   ///< [0, 2], 1 = unchanged
    bool  mirrorH    = false;  ///< flip left/right
    bool  mirrorV    = false;  ///< flip top/bottom

    bool operator== (const ImageAdjustments& o) const noexcept
    {
        return brightness == o.brightness && contrast == o.contrast
            && saturation == o.saturation && mirrorH == o.mirrorH && mirrorV == o.mirrorV;
    }
    bool operator!= (const ImageAdjustments& o) const noexcept { return ! operator== (o); }

    /** True when applying this would leave the image untouched. */
    bool isNeutral() const noexcept
    {
        return brightness == 0.0f && contrast == 0.0f && saturation == 1.0f
            && ! mirrorH && ! mirrorV;
    }

    /** Applies the colour corrections and flips in place. `image` must be a
        writable ARGB image (see applyTo() for the copying variant). */
    void applyInPlace (juce::Image& image) const
    {
        if (! image.isValid() || isNeutral())
            return;

        juce::Image::BitmapData data (image, juce::Image::BitmapData::readWrite);
        const int stride = data.pixelStride;

        if (brightness != 0.0f || contrast != 0.0f || saturation != 1.0f)
        {
            const float k = contrast + 1.0f;
            const float s = saturation;
            const float b = brightness;

            for (int y = 0; y < data.height; ++y)
            {
                juce::uint8* p = data.getLinePointer (y);
                for (int x = 0; x < data.width; ++x, p += stride)
                {
                    float bf = (float) p[0] / 255.0f;
                    float gf = (float) p[1] / 255.0f;
                    float rf = (float) p[2] / 255.0f;

                    const float luma = 0.299f * rf + 0.587f * gf + 0.114f * bf;
                    rf = luma + (rf - luma) * s;
                    gf = luma + (gf - luma) * s;
                    bf = luma + (bf - luma) * s;

                    rf = (rf - 0.5f) * k + 0.5f + b;
                    gf = (gf - 0.5f) * k + 0.5f + b;
                    bf = (bf - 0.5f) * k + 0.5f + b;

                    p[0] = (juce::uint8) juce::jlimit (0, 255, juce::roundToInt (bf * 255.0f));
                    p[1] = (juce::uint8) juce::jlimit (0, 255, juce::roundToInt (gf * 255.0f));
                    p[2] = (juce::uint8) juce::jlimit (0, 255, juce::roundToInt (rf * 255.0f));
                }
            }
        }

        if (mirrorH)
        {
            for (int y = 0; y < data.height; ++y)
            {
                juce::uint8* row = data.getLinePointer (y);
                for (int x = 0; x < data.width / 2; ++x)
                {
                    juce::uint8* left  = row + x * stride;
                    juce::uint8* right = row + (data.width - 1 - x) * stride;
                    for (int c = 0; c < stride; ++c)
                        std::swap (left[c], right[c]);
                }
            }
        }

        if (mirrorV)
        {
            for (int y = 0; y < data.height / 2; ++y)
            {
                juce::uint8* top    = data.getLinePointer (y);
                juce::uint8* bottom = data.getLinePointer (data.height - 1 - y);
                for (int x = 0; x < data.width * stride; ++x)
                    std::swap (top[x], bottom[x]);
            }
        }
    }

    /** Returns `source` converted to ARGB with the adjustments applied. When
        the adjustments are neutral the source is returned as-is (no copy). */
    juce::Image applyTo (const juce::Image& source) const
    {
        if (! source.isValid())
            return source;

        auto argb = source.convertedToFormat (juce::Image::ARGB);
        if (isNeutral())
            return argb;

        auto copy = argb.createCopy();
        applyInPlace (copy);
        return copy;
    }
};

} // namespace fxme
