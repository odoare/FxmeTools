/*
  ------------------------------------------------------------------------------
    IemDecoder.h

    Parser for the IEM AllRADecoder .json export (IEM Plug-in Suite). Reads the
    decoder matrix, the per-row hardware routing and the loudspeaker layout, and
    normalises the matrix to the house convention, so an irregular rig can be fed
    from a decoder designed elsewhere instead of ambi::samplingDecodeMatrix
    (which assumes a reasonably uniform layout).

    Conversions applied so the result decodes an SN3D / ACN (AmbiX) B-format
    feed:
      - if ExpectedInputNormalization is "n3d", each coefficient of ACN degree n
        is multiplied by sqrt(2n+1) to absorb the N3D->SN3D scaling;
      - if Weights is "maxrE" and WeightsAlreadyApplied is false, the per-degree
        max-rE gain is folded in;
      - "imaginary" loudspeakers (IsImaginary) carry no matrix row / routing and
        are simply absent from the result.

    Each result speaker keeps its routing (0-based output), its layout
    azimuth/elevation/radius and its per-speaker Gain, so the caller can apply
    radius (distance) gain/delay compensation and a trim.

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>
#include <vector>

namespace fxme
{

struct IemDecoder
{
    struct Speaker
    {
        int   output      = 0;       // 0-based hardware output (Routing - 1)
        float az          = 0.0f;    // degrees, 0 = front, CCW
        float el          = 0.0f;    // degrees, 0 = horizon, +90 = up
        float radius      = 1.0f;    // metres
        float gainLinear  = 1.0f;    // per-speaker Gain field
        std::vector<float> coeffs;   // numHarmonics decode gains, SN3D + max-rE, signed
    };

    int order        = 0;
    int numHarmonics = 0;            // (order+1)^2
    juce::String name;               // source file name (display)
    juce::String error;              // empty on success
    std::vector<Speaker> speakers;   // real speakers, in matrix-row order

    bool isValid() const noexcept { return error.isEmpty() && ! speakers.empty(); }

    /** Parses an AllRADecoder .json file. On failure the result is invalid and
        `error` describes why. */
    static IemDecoder fromFile (const juce::File& file);

    /** Same, from an already-parsed JSON value (used by the parser and tests). */
    static IemDecoder fromJSON (const juce::var& root, const juce::String& name);
};

} // namespace fxme
