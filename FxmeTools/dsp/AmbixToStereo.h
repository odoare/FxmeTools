/*
  ------------------------------------------------------------------------------
    AmbixToStereo.h

    Renders a first-order AmbiX (ACN/SN3D) stream to stereo through a virtual
    mid/side microphone pair: a cardioid pointed at (azimuth, elevation) for
    the mid, and a horizontal figure-of-eight at azimuth + 90 degrees for the
    side, matrixed as L = M + width * S, R = M - width * S.

    Width 0 collapses to the mono cardioid, 1 is the conventional MS pair, and
    higher values over-widen. The decode is a plain 4-channel matrix, so it is
    allocation-free and the coefficients only need recomputing when an angle
    changes (process() does that once per block, not per sample).

    Only the first four channels (W, Y, Z, X) are read. Higher ambisonic orders
    carry no extra information for first-order microphone patterns and are
    correctly ignored, so a third-order stream can be fed in directly.

    Header-only, depends only on JUCE and dsp/Ambisonics.h.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>
#include "Ambisonics.h"

namespace fxme
{

class AmbixToStereo
{
public:
    AmbixToStereo() = default;

    /** Where the virtual pair points, in degrees. */
    void setAzimuth (float degrees)   { azimuth = degrees; }
    void setElevation (float degrees) { elevation = degrees; }

    /** Side-signal weight: 0 = mono cardioid, 1 = conventional MS pair. */
    void setWidth (float w)           { width = w; }

    /** Linear output gain applied to both channels. */
    void setLevel (float l)           { level = l; }

    float getAzimuth() const noexcept   { return azimuth; }
    float getElevation() const noexcept { return elevation; }
    float getWidth() const noexcept     { return width; }
    float getLevel() const noexcept     { return level; }

    /** Decodes inputBuffer (>= 4 ambisonic channels) and ADDS the result into
        the first two channels of outputBuffer, which is left untouched if
        either buffer is too small. Adding rather than replacing lets several
        sources sum into a shared bus; clear the destination yourself first if
        you want a plain write.

        Realtime safe: no allocation, no locking. */
    void process (const juce::AudioBuffer<float>& inputBuffer,
                  juce::AudioBuffer<float>& outputBuffer)
    {
        if (inputBuffer.getNumChannels() < 4 || outputBuffer.getNumChannels() < 2)
            return;

        const int numSamples = juce::jmin (inputBuffer.getNumSamples(),
                                           outputBuffer.getNumSamples());

        const float azRad = juce::degreesToRadians (azimuth);
        const float elRad = juce::degreesToRadians (elevation);

        // Mid: cardioid on the aiming direction.
        float mid[4];
        ambi::micDecodeWeights (ambi::micCardioid,
                                ambi::directionFromAngles (azRad, elRad),
                                mid);

        // Side: horizontal figure-of-eight, rotated a quarter turn from it.
        float side[4];
        ambi::micDecodeWeights (ambi::micFigure8,
                                ambi::directionFromAngles (azRad + juce::MathConstants<float>::halfPi, 0.0f),
                                side);

        // ACN ordering: 0 = W, 1 = Y, 2 = Z, 3 = X.
        const float* wCh = inputBuffer.getReadPointer (0);
        const float* yCh = inputBuffer.getReadPointer (1);
        const float* zCh = inputBuffer.getReadPointer (2);
        const float* xCh = inputBuffer.getReadPointer (3);

        float* outL = outputBuffer.getWritePointer (0);
        float* outR = outputBuffer.getWritePointer (1);

        for (int i = 0; i < numSamples; ++i)
        {
            const float w = wCh[i], y = yCh[i], z = zCh[i], x = xCh[i];

            const float m = mid[0]  * w + mid[1]  * y + mid[2]  * z + mid[3]  * x;
            const float s = side[0] * w + side[1] * y + side[2] * z + side[3] * x;

            outL[i] += level * (m + width * s);
            outR[i] += level * (m - width * s);
        }
    }

private:
    float azimuth   = 0.0f;
    float elevation = 0.0f;
    float width     = 1.0f;
    float level     = 1.0f;
};

} // namespace fxme
