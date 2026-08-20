/*
  ------------------------------------------------------------------------------
    MicCalibration.h

    Measurement-microphone calibration curve.

    Loads the de-facto-standard plain-text calibration file shipped by REW,
    miniDSP (UMIK-1/2), Dayton (OmniMic / iMM-6) and the FRD tools:

        * comment lines start with '*' or '#'   (e.g. miniDSP "Sens Factor=...")
        freq_Hz   magnitude_dB   [phase_deg]    (whitespace / comma separated)

    The magnitude is the microphone's deviation from flat. To remove it from a
    measurement we DIVIDE the mic response out, i.e. multiply a measured complex
    spectrum by 10^(-mag/20) * e^{-j*phase}. The curve is interpolated in
    log-frequency and held flat (endpoint value) outside the file's range.

    The same loaded curve is meant to be shared globally (one physical mic) and
    applied both to a live SPL / spectrum display and to analysis transfer
    functions; the recorded measurement files themselves stay raw. Owning the
    shared instance and remembering its path is the host application's business
    — this class is just the curve.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>
#include <complex>
#include <vector>

namespace fxme
{

class MicCalibration
{
public:
    MicCalibration() = default;

    /** Parses a calibration file. Returns true on success (>= 2 valid points);
        on failure the previous contents are left cleared. */
    bool loadFromFile (const juce::File& file);

    /** Parses calibration data from raw text (same format as the files). */
    bool loadFromText (const juce::String& text, const juce::String& sourceName = {});

    void clear();

    bool isValid() const noexcept       { return points.size() >= 2; }
    bool hasPhase() const noexcept      { return phaseAvailable; }
    int  getNumPoints() const noexcept  { return (int) points.size(); }
    juce::String getName() const        { return name; }

    /** The raw calibration text this curve was parsed from (file or text
        source), kept verbatim so it can be embedded in measurement manifests
        and read back with loadFromText(). Empty when invalid. */
    juce::String getRawText() const     { return rawText; }
    float getMinFreqHz() const noexcept { return isValid() ? points.front().freqHz : 0.0f; }
    float getMaxFreqHz() const noexcept { return isValid() ? points.back().freqHz  : 0.0f; }

    /** Microphone deviation from flat (dB) at a frequency, interpolated in
        log-frequency; endpoints held outside the file range. 0 when invalid. */
    float magnitudeDbAt (double freqHz) const;

    /** Microphone phase (radians) at a frequency. 0 when invalid or the file
        carries no phase column. */
    float phaseRadAt (double freqHz) const;

    /** Complex factor that divides the mic response out of a measurement:
        10^(-mag/20) * e^{-j*phase}. Returns 1 when invalid. Multiply each
        measured spectrum bin by this. */
    std::complex<float> correctionAt (double freqHz) const;

    /** Applies the correction in place to a half-spectrum whose bin k maps to
        frequency k*sampleRate/fftSize. No-op when invalid. */
    void applyToSpectrum (std::vector<std::complex<float>>& spec,
                          double sampleRate, int fftSize) const;

private:
    struct Point { float freqHz; float magDb; float phaseRad; };

    // Interpolates magnitude (dB) and phase (rad) at freqHz. Assumes isValid().
    void interp (double freqHz, float& magDb, float& phaseRad) const;

    std::vector<Point> points;          // sorted by frequency; phase unwrapped
    bool phaseAvailable = false;
    juce::String name;                  // source file name (for display)
    juce::String rawText;               // verbatim source text (for embedding)

    JUCE_LEAK_DETECTOR (MicCalibration)
};

} // namespace fxme
