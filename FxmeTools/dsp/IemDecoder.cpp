/*
  ------------------------------------------------------------------------------
    IemDecoder.cpp

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include "IemDecoder.h"
#include <FxmeTools/dsp/Ambisonics.h>    // ambi::channelsForOrder / orderOfChannel / maxREGain
#include <cmath>

namespace fxme
{

IemDecoder IemDecoder::fromFile (const juce::File& file)
{
    if (! file.existsAsFile())
    {
        IemDecoder d;
        d.error = "File not found.";
        return d;
    }
    return fromJSON (juce::JSON::parse (file.loadFileAsString()), file.getFileName());
}

IemDecoder IemDecoder::fromJSON (const juce::var& root, const juce::String& name)
{
    IemDecoder d;
    d.name = name;

    if (! root.isObject())              { d.error = "Not a JSON object.";        return d; }
    auto decoder = root["Decoder"];
    if (! decoder.isObject())           { d.error = "No \"Decoder\" object.";    return d; }

    auto* matrix  = decoder["Matrix"].getArray();
    auto* routing = decoder["Routing"].getArray();
    if (matrix == nullptr || matrix->isEmpty())            { d.error = "No decoder matrix.";        return d; }
    if (routing == nullptr || routing->size() != matrix->size())
                                                           { d.error = "Routing missing or size mismatch."; return d; }

    auto* row0 = (*matrix)[0].getArray();
    if (row0 == nullptr || row0->isEmpty())                { d.error = "Empty matrix row.";         return d; }

    const int L = matrix->size();
    const int H = row0->size();
    const int order = (int) std::lround (std::sqrt ((double) H)) - 1;
    if (order < 1 || ambi::channelsForOrder (order) != H)
    {
        d.error = "Matrix has " + juce::String (H) + " columns, not a square Ambisonic order.";
        return d;
    }
    d.order = order;
    d.numHarmonics = H;

    const auto norm    = decoder["ExpectedInputNormalization"].toString().trim().toLowerCase();
    const bool n3d     = norm == "n3d";
    const auto weights = decoder["Weights"].toString().trim().toLowerCase();
    const bool maxRe   = (weights == "maxre") && ! (bool) decoder["WeightsAlreadyApplied"];

    // Loudspeaker layout (top-level in newer files, under Decoder in older
    // ones), keyed by 1-based Channel.
    auto layout = root["LoudspeakerLayout"];
    if (! layout.isObject())
        layout = decoder["LoudspeakerLayout"];
    auto* spk = layout["Loudspeakers"].getArray();

    auto findSpeaker = [spk] (int channel) -> juce::var
    {
        if (spk != nullptr)
            for (const auto& s : *spk)
                if ((int) s["Channel"] == channel)
                    return s;
        return {};
    };

    // Per-column factor folding N3D->SN3D and/or max-rE into the coefficients.
    auto colFactor = [&] (int j)
    {
        const int n = ambi::orderOfChannel (j);
        double f = 1.0;
        if (n3d)   f *= std::sqrt (2.0 * (double) n + 1.0);
        if (maxRe) f *= ambi::maxREGain (order, n);
        return f;
    };

    for (int r = 0; r < L; ++r)
    {
        auto* mrow = (*matrix)[r].getArray();
        if (mrow == nullptr || mrow->size() != H) { d.error = "Ragged decoder matrix."; return d; }

        const int channel = (int) (*routing)[r];        // 1-based hardware channel
        Speaker sp;
        sp.output = channel - 1;

        const auto ls = findSpeaker (channel);
        if (ls.isObject())
        {
            sp.az     = (float) (double) ls["Azimuth"];
            sp.el     = (float) (double) ls["Elevation"];
            sp.radius = (float) (double) ls["Radius"];
            if (ls.hasProperty ("Gain"))
                sp.gainLinear = (float) (double) ls["Gain"];
        }
        if (sp.radius <= 0.0f)
            sp.radius = 1.0f;

        sp.coeffs.resize ((size_t) H);
        for (int j = 0; j < H; ++j)
            sp.coeffs[(size_t) j] = (float) ((double) (*mrow)[j] * colFactor (j));

        d.speakers.push_back (std::move (sp));
    }

    return d;
}

} // namespace fxme
