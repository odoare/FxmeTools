/*
  ------------------------------------------------------------------------------
    ColorBlobTracker.h

    Colour-keyed centroid tracking on a juce::Image: one pass over the
    pixels computes the weighted centre of gravity of everything close to a
    reference colour, plus a confidence value. This is the "easy method" of
    object tracking — mark the object with a distinctive colour, sample that
    colour, follow its centroid frame by frame. Multiple objects are tracked
    by calling track() once per reference colour.

    Matching: each pixel gets a weight from its RGB distance to the
    reference (Euclidean in the unit cube, normalised so the maximum
    possible distance is 1), with a quadratic falloff inside the tolerance
    radius and zero outside:

        w = max (0, 1 - (d / tolerance)^2)

    The result is the weight-averaged pixel position in normalised image
    coordinates ([0,1]^2, origin top-left, full image span).

    Detection and confidence both derive from the total weighted mass
    (blob size times match quality, in pixels): found when the mass reaches
    minTotalWeight, confidence saturating at 10x that. The thresholds are
    absolute pixel counts, so they assume a smallish analysis image (a few
    hundred pixels a side, e.g. VideoEngine's working frame) rather than a
    full camera resolution.

    Cost: one read pass over the image per call (no allocation). Message
    thread — like everything image-side.

    Header-only, depends on juce_graphics only. Known user: Localizer's
    ColorFollower tracking module.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_graphics/juce_graphics.h>
#include <cmath>

namespace fxme
{

class ColorBlobTracker
{
public:
    struct Params
    {
        juce::Colour reference;          ///< the colour to follow
        float tolerance = 0.15f;         ///< matching radius, fraction of the max RGB distance
        float minTotalWeight = 4.0f;     ///< weighted-pixel mass below which nothing is "found"
    };

    struct Result
    {
        bool found = false;
        float x = 0.5f, y = 0.5f;        ///< centroid, normalised [0,1], origin top-left
        float confidence = 0.0f;         ///< 0..1, saturates at 10x minTotalWeight of mass
    };

    /** One pass over `image`. Invalid images and empty matches return
        found = false with the centre as a harmless placeholder position. */
    static Result track (const juce::Image& image, const Params& params)
    {
        Result result;

        if (! image.isValid() || image.getWidth() < 2 || image.getHeight() < 2)
            return result;

        const float refR = params.reference.getFloatRed();
        const float refG = params.reference.getFloatGreen();
        const float refB = params.reference.getFloatBlue();

        // Squared-distance threshold; distances are normalised so that the
        // farthest colour (black to white) is exactly 1.
        const float tolerance = juce::jmax (1.0e-3f, params.tolerance);
        const float tol2 = tolerance * tolerance;
        constexpr float distanceNorm2 = 1.0f / 3.0f;    // 1 / (sqrt(3))^2

        juce::Image::BitmapData data (image, juce::Image::BitmapData::readOnly);
        const int stride = data.pixelStride;

        double sumW = 0.0, sumX = 0.0, sumY = 0.0;

        for (int y = 0; y < data.height; ++y)
        {
            const juce::uint8* p = data.getLinePointer (y);

            for (int x = 0; x < data.width; ++x, p += stride)
            {
                // JUCE packs both RGB and ARGB images as B, G, R [, A].
                const float db = (float) p[0] / 255.0f - refB;
                const float dg = (float) p[1] / 255.0f - refG;
                const float dr = (float) p[2] / 255.0f - refR;

                const float d2 = (dr * dr + dg * dg + db * db) * distanceNorm2;

                if (d2 < tol2)
                {
                    const double w = 1.0 - (double) (d2 / tol2);
                    sumW += w;
                    sumX += w * x;
                    sumY += w * y;
                }
            }
        }

        const float minWeight = juce::jmax (1.0e-3f, params.minTotalWeight);

        if (sumW < (double) minWeight)
            return result;

        result.found = true;
        result.x = (float) (sumX / sumW) / (float) (data.width  - 1);
        result.y = (float) (sumY / sumW) / (float) (data.height - 1);
        result.confidence = juce::jmin (1.0f, (float) sumW / (10.0f * minWeight));
        return result;
    }
};

} // namespace fxme
