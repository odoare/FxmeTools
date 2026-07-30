/*
  ------------------------------------------------------------------------------
    ColorBlobTracker.h

    Colour-keyed blob tracking on a juce::Image: find the connected region
    whose colour best matches a reference, and report its centroid. Mark an
    object with a distinctive colour, sample that colour, follow the blob.
    Several objects are tracked by calling track() once per reference colour.

    Two design points matter, both learned from a first version that used a
    plain RGB distance and one global centroid:

    1. Matching ignores brightness (MatchMode::chroma, the default).
       Euclidean distance in RGB counts a brightness difference exactly as
       much as a hue difference, so against a pure red reference a warm
       bright area (1.0, 0.3, 0.2) scores *better* than the real red object
       seen in shadow (0.4, 0, 0), and no tolerance separates them. Chroma
       mode compares normalised chromaticity (r, g over r+g+b), which is
       invariant to illumination level, and guards against the two cases
       where chromaticity is meaningless: near-black pixels (no signal) and
       near-grey pixels (hue is noise). A reference colour that is itself
       unsaturated falls back to RGB matching automatically, so tracking a
       white or grey object still works.

    2. The result is one connected blob, not a global centre of gravity.
       Summing every matching pixel into one centroid lets area beat
       quality: a few hundred weakly matching background pixels outweigh a
       few dozen strong ones on the object, and the reported position sits
       between them or on the background. Here the mask is split into
       connected components (two-pass union-find, 8-connectivity), each is
       scored on its own, and only the winner's centroid is reported. Size
       and shape filters (minBlobPixels, maxBlobFraction, minCompactness)
       then mean what they say, because they apply per blob.

    Optional temporal gating (previousX/Y + searchRadius) biases the score
    towards blobs near the last known position, which is what stops a
    momentary false positive from stealing an established track. It is a
    soft preference, not a hard rejection, so a fast-moving object is not
    lost.

    Cost: two passes over the image plus the labelling, all on the small
    analysis frame (a few hundred pixels a side). Scratch buffers are
    members and are reused, so the steady state does not allocate; this is
    why track() is an instance method rather than a static one. Message
    thread, like everything image-side.

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
#include <vector>

namespace fxme
{

class ColorBlobTracker
{
public:
    /** How a pixel is compared to the reference colour. */
    enum class MatchMode
    {
        chroma = 0,   ///< normalised chromaticity, brightness ignored (default)
        rgb           ///< plain RGB distance (brightness counts)
    };

    struct Params
    {
        juce::Colour reference;                 ///< the colour to follow
        MatchMode matchMode = MatchMode::chroma;

        /** Matching radius. Both modes normalise their distance so that 1 is
            the largest possible difference, so the scale is comparable. */
        float tolerance = 0.15f;

        /** Chroma-mode guards: pixels darker than minValue (mean channel) or
            less saturated than minSaturation never match, because their hue
            is respectively absent and noise-dominated. */
        float minValue = 0.06f;
        float minSaturation = 0.12f;

        /** Per-blob acceptance. minBlobPixels rejects speckle, maxBlobFraction
            rejects regions so large they are background rather than an object,
            minCompactness (blob pixels over its bounding-box area) rejects
            smears such as a lit wall while accepting any convex-ish marker. */
        int   minBlobPixels = 12;
        float maxBlobFraction = 0.35f;
        float minCompactness = 0.15f;

        /** Temporal gating: when hasPrevious and searchRadius > 0, a blob's
            score is divided by 1 + (distance / searchRadius)^2, so near blobs
            win ties without far ones being impossible. */
        bool  hasPrevious = false;
        float previousX = 0.5f, previousY = 0.5f;
        float searchRadius = 0.0f;

        /** Blob size (in pixels) at which the size part of the confidence
            saturates. */
        int confidencePixels = 60;
    };

    /** One connected candidate region. Positions and bounds are normalised
        ([0,1]^2, origin top-left). */
    struct Blob
    {
        float x = 0.5f, y = 0.5f;
        juce::Rectangle<float> bounds;
        int   pixels = 0;
        float mass = 0.0f;        ///< sum of match weights (pixels x quality)
        float score = 0.0f;       ///< quality x size, then gated (see track())
        bool  accepted = false;   ///< passed the size and shape filters
        bool  chosen = false;     ///< the winner
    };

    struct Result
    {
        bool  found = false;
        float x = 0.5f, y = 0.5f;      ///< centroid of the chosen blob
        float confidence = 0.0f;       ///< match quality x size, both in [0,1]
        int   blobPixels = 0;          ///< size of the chosen blob
        int   candidates = 0;          ///< blobs that passed the filters
        juce::Rectangle<float> bounds; ///< chosen blob's bounding box
    };

    /** Optional per-call diagnostics for a UI. `blobs` always lists every
        candidate (accepted or not) when a Diagnostics is passed; `mask` is
        only rendered when wantMask is set. */
    struct Diagnostics
    {
        bool wantMask = false;
        juce::Image mask;              ///< working-resolution overlay, alpha = match weight
        std::vector<Blob> blobs;
    };

    //==========================================================================
    Result track (const juce::Image& image, const Params& params,
                  Diagnostics* diagnostics = nullptr)
    {
        Result result;

        if (diagnostics != nullptr)
            diagnostics->blobs.clear();

        if (! image.isValid() || image.getWidth() < 2 || image.getHeight() < 2)
            return result;

        const int w = image.getWidth();
        const int h = image.getHeight();

        computeWeights (image, params, w, h);

        if (diagnostics != nullptr && diagnostics->wantMask)
            renderMask (*diagnostics, w, h);

        label (w, h);
        gather (params, w, h);

        // Pick the best accepted blob.
        const Blob* best = nullptr;

        for (auto& blob : blobs)
        {
            if (! blob.accepted)
                continue;

            ++result.candidates;

            if (best == nullptr || blob.score > best->score)
                best = &blob;
        }

        if (best != nullptr)
        {
            result.found      = true;
            result.x          = best->x;
            result.y          = best->y;
            result.blobPixels = best->pixels;
            result.bounds     = best->bounds;
            result.confidence = juce::jlimit (0.0f, 1.0f,
                                              quality (*best) * sizeFactor (*best, params));
        }

        if (diagnostics != nullptr)
        {
            for (auto& blob : blobs)
                blob.chosen = (&blob == best);

            diagnostics->blobs = blobs;
        }

        return result;
    }

private:
    //==========================================================================
    /** Fills `weights` with the per-pixel match weight (0 = no match). */
    void computeWeights (const juce::Image& image, const Params& params, int w, int h)
    {
        weights.assign ((size_t) (w * h), 0.0f);

        const float refR = params.reference.getFloatRed();
        const float refG = params.reference.getFloatGreen();
        const float refB = params.reference.getFloatBlue();

        const float tolerance = juce::jmax (1.0e-3f, params.tolerance);
        const float tol2 = tolerance * tolerance;

        // Chroma matching needs a saturated reference to be meaningful; a grey
        // or white target falls back to RGB, where brightness is the signal.
        const float refSum = refR + refG + refB;
        const float refSat = refSum > 1.0e-4f
                                 ? 1.0f - 3.0f * juce::jmin (refR, juce::jmin (refG, refB)) / refSum
                                 : 0.0f;
        const bool useChroma = params.matchMode == MatchMode::chroma
                                && refSat >= params.minSaturation;

        const float refCr = refSum > 1.0e-4f ? refR / refSum : 0.0f;
        const float refCg = refSum > 1.0e-4f ? refG / refSum : 0.0f;

        // Normalisers so both modes report a distance in [0, 1]: the longest
        // RGB diagonal is sqrt(3), the widest chromaticity separation sqrt(2).
        constexpr float rgbNorm2    = 1.0f / 3.0f;
        constexpr float chromaNorm2 = 1.0f / 2.0f;

        const juce::Image::BitmapData data (image, juce::Image::BitmapData::readOnly);
        const int stride = data.pixelStride;

        for (int y = 0; y < h; ++y)
        {
            const juce::uint8* p = data.getLinePointer (y);
            float* out = weights.data() + (size_t) y * (size_t) w;

            for (int x = 0; x < w; ++x, p += stride)
            {
                // JUCE packs both RGB and ARGB as B, G, R [, A].
                const float b = (float) p[0] / 255.0f;
                const float g = (float) p[1] / 255.0f;
                const float r = (float) p[2] / 255.0f;

                float d2;

                if (useChroma)
                {
                    const float sum = r + g + b;

                    if (sum < 3.0f * params.minValue)
                        continue;                       // too dark to have a colour

                    const float sat = 1.0f - 3.0f * juce::jmin (r, juce::jmin (g, b)) / sum;

                    if (sat < params.minSaturation)
                        continue;                       // grey: hue is noise

                    const float dcr = r / sum - refCr;
                    const float dcg = g / sum - refCg;
                    d2 = (dcr * dcr + dcg * dcg) * chromaNorm2;
                }
                else
                {
                    const float dr = r - refR, dg = g - refG, db = b - refB;
                    d2 = (dr * dr + dg * dg + db * db) * rgbNorm2;
                }

                if (d2 < tol2)
                    out[x] = 1.0f - d2 / tol2;
            }
        }
    }

    void renderMask (Diagnostics& diagnostics, int w, int h)
    {
        if (diagnostics.mask.getWidth() != w || diagnostics.mask.getHeight() != h)
            diagnostics.mask = juce::Image (juce::Image::ARGB, w, h, true);

        juce::Image::BitmapData data (diagnostics.mask, juce::Image::BitmapData::writeOnly);

        for (int y = 0; y < h; ++y)
        {
            juce::uint8* p = data.getLinePointer (y);
            const float* in = weights.data() + (size_t) y * (size_t) w;

            for (int x = 0; x < w; ++x, p += data.pixelStride)
            {
                // Premultiplied white: all four components equal the alpha.
                const auto a = (juce::uint8) juce::jlimit (0, 255, juce::roundToInt (in[x] * 255.0f));
                p[0] = p[1] = p[2] = a;

                if (data.pixelStride == 4)
                    p[3] = a;
            }
        }
    }

    //==========================================================================
    /** Mean match weight over the blob, in [0,1]: how well its colour fits. */
    static float quality (const Blob& blob)
    {
        return blob.pixels > 0 ? blob.mass / (float) blob.pixels : 0.0f;
    }

    /** Size term, saturating at confidencePixels so that a big blob gains
        nothing further from being bigger. */
    static float sizeFactor (const Blob& blob, const Params& params)
    {
        return juce::jmin (1.0f, (float) blob.pixels
                                     / (float) juce::jmax (1, params.confidencePixels));
    }

    int findRoot (int label)
    {
        while (parent[(size_t) label] != label)
        {
            parent[(size_t) label] = parent[(size_t) parent[(size_t) label]];   // path halving
            label = parent[(size_t) label];
        }

        return label;
    }

    void unite (int a, int b)
    {
        a = findRoot (a);
        b = findRoot (b);

        if (a != b)
            parent[(size_t) juce::jmax (a, b)] = juce::jmin (a, b);
    }

    /** Two-pass connected-component labelling, 8-connectivity. Label 0 is
        the background. */
    void label (int w, int h)
    {
        labels.assign ((size_t) (w * h), 0);
        parent.assign (1, 0);

        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                const size_t index = (size_t) y * (size_t) w + (size_t) x;

                if (weights[index] <= 0.0f)
                    continue;

                // Already-visited 8-neighbours: W, NW, N, NE.
                int found = 0;

                const auto consider = [&] (int nx, int ny)
                {
                    if (nx < 0 || ny < 0 || nx >= w)
                        return;

                    const int other = labels[(size_t) ny * (size_t) w + (size_t) nx];

                    if (other == 0)
                        return;

                    if (found == 0)
                        found = other;
                    else
                        unite (found, other);
                };

                consider (x - 1, y);
                consider (x - 1, y - 1);
                consider (x,     y - 1);
                consider (x + 1, y - 1);

                if (found == 0)
                {
                    found = (int) parent.size();
                    parent.push_back (found);
                }

                labels[index] = found;
            }
        }
    }

    /** Resolves the labels and accumulates one Blob per component, applying
        the size, shape and gating rules. */
    void gather (const Params& params, int w, int h)
    {
        blobs.clear();

        if (parent.size() <= 1)
            return;

        // Root label -> index into blobs.
        indexOfRoot.assign (parent.size(), -1);

        const float spanX = (float) juce::jmax (1, w - 1);
        const float spanY = (float) juce::jmax (1, h - 1);

        accums.clear();

        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                const size_t index = (size_t) y * (size_t) w + (size_t) x;
                const int raw = labels[index];

                if (raw == 0)
                    continue;

                const int root = findRoot (raw);
                int slot = indexOfRoot[(size_t) root];

                if (slot < 0)
                {
                    slot = (int) accums.size();
                    indexOfRoot[(size_t) root] = slot;
                    Accum fresh;
                    fresh.minX = fresh.maxX = x;
                    fresh.minY = fresh.maxY = y;
                    accums.push_back (fresh);
                }

                auto& a = accums[(size_t) slot];
                const double weight = (double) weights[index];

                a.mass += weight;
                a.sumX += weight * x;
                a.sumY += weight * y;
                ++a.pixels;
                a.minX = juce::jmin (a.minX, x);
                a.maxX = juce::jmax (a.maxX, x);
                a.minY = juce::jmin (a.minY, y);
                a.maxY = juce::jmax (a.maxY, y);
            }
        }

        const int totalPixels = w * h;
        const int maxPixels = juce::roundToInt (params.maxBlobFraction * (float) totalPixels);

        for (const auto& a : accums)
        {
            if (a.mass <= 0.0)
                continue;

            Blob blob;
            blob.pixels = a.pixels;
            blob.mass   = (float) a.mass;
            blob.x      = (float) (a.sumX / a.mass) / spanX;
            blob.y      = (float) (a.sumY / a.mass) / spanY;
            blob.bounds = juce::Rectangle<float>::leftTopRightBottom (
                              (float) a.minX / spanX, (float) a.minY / spanY,
                              (float) a.maxX / spanX, (float) a.maxY / spanY);

            const int boxArea = (a.maxX - a.minX + 1) * (a.maxY - a.minY + 1);
            const float compactness = boxArea > 0 ? (float) a.pixels / (float) boxArea : 0.0f;

            blob.accepted = a.pixels >= params.minBlobPixels
                             && a.pixels <= maxPixels
                             && compactness >= params.minCompactness;

            // Score = match quality x size, with size saturating at
            // confidencePixels. Scoring by mass alone (area x quality) would
            // let a large mediocre region outrank a small perfect one, which
            // is the failure this class exists to avoid; saturating the size
            // term means that past "big enough", colour quality decides,
            // while speckle still loses on the size term.
            blob.score = quality (blob) * sizeFactor (blob, params);

            if (blob.accepted && params.hasPrevious && params.searchRadius > 0.0f)
            {
                const float dx = blob.x - params.previousX;
                const float dy = blob.y - params.previousY;
                const float d  = std::sqrt (dx * dx + dy * dy) / params.searchRadius;
                blob.score /= 1.0f + d * d;
            }

            blobs.push_back (blob);
        }
    }

    //==========================================================================
    std::vector<float> weights;
    std::vector<int> labels, parent, indexOfRoot;
    std::vector<Blob> blobs;

    struct Accum
    {
        double mass = 0.0, sumX = 0.0, sumY = 0.0;
        int pixels = 0, minX = 0, minY = 0, maxX = 0, maxY = 0;
    };
    std::vector<Accum> accums;
};

} // namespace fxme
