/*
  ------------------------------------------------------------------------------
    MicCalibration.cpp

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include "MicCalibration.h"
#include <algorithm>
#include <cmath>

namespace fxme
{

void MicCalibration::clear()
{
    points.clear();
    phaseAvailable = false;
    name = {};
    rawText = {};
}

bool MicCalibration::loadFromFile (const juce::File& file)
{
    if (! file.existsAsFile())
        return false;
    return loadFromText (file.loadFileAsString(), file.getFileName());
}

bool MicCalibration::loadFromText (const juce::String& text, const juce::String& sourceName)
{
    std::vector<Point> parsed;
    bool sawPhaseColumn = false;
    bool firstDataLine = true;

    juce::StringArray lines;
    lines.addLines (text);

    for (auto rawLine : lines)
    {
        auto line = rawLine.trim();
        if (line.isEmpty() || line.startsWithChar ('*') || line.startsWithChar ('#')
            || line.startsWithChar (';'))
            continue;

        // Tokens separated by whitespace or commas; drop empties.
        juce::StringArray tok;
        tok.addTokens (line, " \t,", "");
        tok.removeEmptyStrings();
        if (tok.size() < 2)
            continue;

        // A line must start with a number; otherwise treat it as a stray header.
        const juce::String& f = tok[0];
        if (! (f.containsAnyOf ("0123456789") && f.getDoubleValue() > 0.0))
            continue;

        Point p;
        p.freqHz   = (float) f.getDoubleValue();
        p.magDb    = (float) tok[1].getDoubleValue();
        const bool hasThird = tok.size() >= 3;
        p.phaseRad = hasThird ? juce::degreesToRadians ((float) tok[2].getDoubleValue()) : 0.0f;

        if (firstDataLine) { sawPhaseColumn = hasThird; firstDataLine = false; }
        parsed.push_back (p);
    }

    if (parsed.size() < 2)
        return false;

    std::sort (parsed.begin(), parsed.end(),
               [] (const Point& a, const Point& b) { return a.freqHz < b.freqHz; });

    // Unwrap the phase across frequency so later linear interpolation is correct
    // even where the file wrapped at +/-180 degrees.
    if (sawPhaseColumn)
    {
        for (size_t i = 1; i < parsed.size(); ++i)
        {
            float d = parsed[i].phaseRad - parsed[i - 1].phaseRad;
            while (d >  juce::MathConstants<float>::pi) { parsed[i].phaseRad -= juce::MathConstants<float>::twoPi; d -= juce::MathConstants<float>::twoPi; }
            while (d < -juce::MathConstants<float>::pi) { parsed[i].phaseRad += juce::MathConstants<float>::twoPi; d += juce::MathConstants<float>::twoPi; }
        }
    }

    points         = std::move (parsed);
    phaseAvailable = sawPhaseColumn;
    name           = sourceName;
    rawText        = text;
    return true;
}

void MicCalibration::interp (double freqHz, float& magDb, float& phaseRad) const
{
    // Clamp to the endpoints outside the measured range (held flat).
    if (freqHz <= (double) points.front().freqHz)
    {
        magDb    = points.front().magDb;
        phaseRad = points.front().phaseRad;
        return;
    }
    if (freqHz >= (double) points.back().freqHz)
    {
        magDb    = points.back().magDb;
        phaseRad = points.back().phaseRad;
        return;
    }

    // Bracket the frequency (points are sorted ascending).
    const auto it = std::lower_bound (points.begin(), points.end(), (float) freqHz,
                                      [] (const Point& p, float f) { return p.freqHz < f; });
    const auto& hi = *it;
    const auto& lo = *(it - 1);

    // Linear interpolation in log-frequency (guard against duplicate freqs).
    const double denom = std::log ((double) hi.freqHz / (double) lo.freqHz);
    const double t = denom > 0.0 ? std::log (freqHz / (double) lo.freqHz) / denom : 0.0;
    magDb    = lo.magDb    + (float) t * (hi.magDb    - lo.magDb);
    phaseRad = lo.phaseRad + (float) t * (hi.phaseRad - lo.phaseRad);
}

float MicCalibration::magnitudeDbAt (double freqHz) const
{
    if (! isValid())
        return 0.0f;
    float m, p;
    interp (freqHz, m, p);
    return m;
}

float MicCalibration::phaseRadAt (double freqHz) const
{
    if (! isValid() || ! phaseAvailable)
        return 0.0f;
    float m, p;
    interp (freqHz, m, p);
    return p;
}

std::complex<float> MicCalibration::correctionAt (double freqHz) const
{
    if (! isValid())
        return { 1.0f, 0.0f };

    float magDb, phaseRad;
    interp (freqHz, magDb, phaseRad);
    if (! phaseAvailable)
        phaseRad = 0.0f;

    // Divide the mic response out: gain = 10^(-mag/20), phase = -mic phase.
    const float gain = std::pow (10.0f, -magDb / 20.0f);
    return std::polar (gain, -phaseRad);
}

void MicCalibration::applyToSpectrum (std::vector<std::complex<float>>& spec,
                                      double sampleRate, int fftSize) const
{
    if (! isValid() || sampleRate <= 0.0 || fftSize <= 0 || spec.empty())
        return;

    for (size_t k = 0; k < spec.size(); ++k)
    {
        const double f = (double) k * sampleRate / (double) fftSize;
        spec[k] *= correctionAt (f);
    }
}

} // namespace fxme
