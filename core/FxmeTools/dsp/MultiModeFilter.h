/*
  ------------------------------------------------------------------------------
    dsp/MultiModeFilter.h

    One filter, many types, all on the trapezoidal SVF (fxme::TptSvf), so the
    cutoff and every other setting can be modulated at control rate without
    the bursts a direct-form biquad emits when swept. Built for synth voices.

      lowPass12 / lowPass24     one or two SVF sections (the second carries
      highPass12 / highPass24   the resonance; the first is Butterworth)
      bandPass                  normalised: 0 dB at the centre
      notch
      peak                      bell, gain in dB, Q from the resonance
      lowShelf / highShelf      gain in dB (Cytomic's SVF shelves)
      formant                   three band-passes on the formants of a vowel,
                                blended between two vowels (A E I O U) by a
                                position in [0, 1]; resonance narrows them

    Resonance is 0 .. 1 for every type: Q goes from 0.707 (or the formant's
    natural bandwidth) up to about 22.

    Channels: one instance per channel. setParameters() is meant for control
    rate (every 16 to 64 samples): it computes tangents and exponentials;
    hasParameters() tells when it can be skipped, and copySettingsFrom()
    sets a second channel up from the first without computing them again.
    processSample() is cheap. Header-only, no allocation, realtime safe.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/dsp/TptSvf.h>
#include <FxmeTools/util/Math.h>
#include <cmath>

namespace fxme
{

class MultiModeFilter
{
public:
    enum class Type : int
    {
        lowPass12 = 0, lowPass24, highPass12, highPass24, bandPass, notch,
        peak, lowShelf, highShelf, formant
    };

    static constexpr const char* const typeNames[] = { "LP 12", "LP 24", "HP 12", "HP 24", "Band pass",
                                                       "Notch", "Peak", "Low shelf", "High shelf", "Formant" };
    static constexpr int numTypes = (int) (sizeof (typeNames) / sizeof (typeNames[0]));

    static constexpr const char* const vowelNames[] = { "A", "E", "I", "O", "U" };
    static constexpr int numVowels = 5;

    struct Parameters
    {
        Type type = Type::lowPass12;
        float cutoffHz = 1000.0f;
        float resonance = 0.0f;   // 0 .. 1
        float gainDb = 0.0f;      // peak and shelves
        int vowelA = 0, vowelB = 1;
        float vowelPosition = 0.0f;
    };

    void prepare (double sampleRate) noexcept
    {
        sr = sampleRate > 0.0 ? sampleRate : 44100.0;
        reset();
        setParameters (params);
    }

    void reset() noexcept
    {
        for (auto& s : sections)
            s.reset();
    }

    void setParameters (const Parameters& p) noexcept
    {
        if (p.type != params.type)
            reset();
        params = p;

        const float res = jlimit (0.0f, 1.0f, p.resonance);
        const double q = 0.70710678 * std::pow (2.0, 5.0 * (double) res);   // 0.707 .. 22.6
        const double fc = jlimit (10.0, 0.49 * sr, (double) p.cutoffHz);

        switch (p.type)
        {
            case Type::lowPass12:
            case Type::highPass12:
            case Type::bandPass:
            case Type::notch:
                sections[0].setup (TptSvf::gFor (fc, sr), 1.0 / q);
                break;

            case Type::lowPass24:
            case Type::highPass24:
            {
                const double g = TptSvf::gFor (fc, sr);
                sections[0].setup (g, SvfPassCascade::butterworthK (0, 2));
                sections[1].setup (g, SvfPassCascade::butterworthK (1, 2) / std::pow (2.0, 5.0 * (double) res));
                break;
            }

            case Type::peak:
            {
                const double A = std::pow (10.0, (double) p.gainDb / 40.0);
                sections[0].setup (TptSvf::gFor (fc, sr), 1.0 / (q * A));
                m0 = 1.0; m1 = (1.0 / (q * A)) * (A * A - 1.0); m2 = 0.0;
                break;
            }

            case Type::lowShelf:
            {
                const double A = std::pow (10.0, (double) p.gainDb / 40.0);
                const double k = 1.0 / q;
                sections[0].setup (TptSvf::gFor (fc, sr) / std::sqrt (A), k);
                m0 = 1.0; m1 = k * (A - 1.0); m2 = A * A - 1.0;
                break;
            }

            case Type::highShelf:
            {
                const double A = std::pow (10.0, (double) p.gainDb / 40.0);
                const double k = 1.0 / q;
                sections[0].setup (jmin (TptSvf::gFor (fc, sr) * std::sqrt (A), 1.0e4), k);
                m0 = A * A; m1 = k * (1.0 - A) * A; m2 = 1.0 - A * A;
                break;
            }

            case Type::formant:
            {
                const int a = jlimit (0, numVowels - 1, p.vowelA);
                const int b = jlimit (0, numVowels - 1, p.vowelB);
                const float t = jlimit (0.0f, 1.0f, p.vowelPosition);
                const double narrow = 1.0 + 6.0 * (double) res;
                for (int f = 0; f < numFormants; ++f)
                {
                    const double hz = (double) (formants[a][f] + (formants[b][f] - formants[a][f]) * t);
                    const double bw = (double) bandwidths[f] / narrow;
                    const double gDb = (double) (gainsDb[a][f] + (gainsDb[b][f] - gainsDb[a][f]) * t);
                    sections[f].setup (TptSvf::gFor (jmin (hz, 0.45 * sr), sr), bw / jmax (1.0, hz));
                    formantGain[f] = (float) std::pow (10.0, gDb / 20.0);
                }
                break;
            }
        }
    }

    const Parameters& getParameters() const noexcept { return params; }

    /** True when `p` would leave the filter as it is (setParameters() can be
        skipped). */
    bool hasParameters (const Parameters& p) const noexcept
    {
        auto same = [] (float a, float b) { return ! (a < b || a > b); };
        return p.type == params.type && same (p.cutoffHz, params.cutoffHz) && same (p.resonance, params.resonance)
            && same (p.gainDb, params.gainDb) && p.vowelA == params.vowelA && p.vowelB == params.vowelB
            && same (p.vowelPosition, params.vowelPosition);
    }

    /** Another instance's settings (type, coefficients, gains), keeping this
        one's state: the second channel of a pair, set up without computing
        the coefficients again. Both must share the sample rate. */
    void copySettingsFrom (const MultiModeFilter& other) noexcept
    {
        if (other.params.type != params.type)
            reset();
        params = other.params;
        for (int f = 0; f < numFormants; ++f)
        {
            sections[f].copyCoefficientsFrom (other.sections[f]);
            formantGain[f] = other.formantGain[f];
        }
        m0 = other.m0;
        m1 = other.m1;
        m2 = other.m2;
    }

    float processSample (float x) noexcept
    {
        switch (params.type)
        {
            case Type::lowPass12:  return sections[0].tick (x).low;
            case Type::highPass12: return sections[0].tick (x).high;
            case Type::bandPass:   return (float) sections[0].getK() * sections[0].tick (x).band;
            case Type::notch:
            {
                const auto o = sections[0].tick (x);
                return o.low + o.high;
            }
            case Type::lowPass24:  return sections[1].tick (sections[0].tick (x).low).low;
            case Type::highPass24: return sections[1].tick (sections[0].tick (x).high).high;
            case Type::peak:
            case Type::lowShelf:
            case Type::highShelf:
            {
                const auto o = sections[0].tick (x);
                return (float) (m0 * (double) x + m1 * (double) o.band + m2 * (double) o.low);
            }
            case Type::formant:
            {
                float y = 0.0f;
                for (int f = 0; f < numFormants; ++f)
                    y += formantGain[f] * (float) sections[f].getK() * sections[f].tick (x).band;
                return y * formantMakeup;
            }
        }
        return x;
    }

private:
    static constexpr int numFormants = 3;

    // Formants of sung vowels (tenor), Hz, and their relative levels, dB.
    static constexpr float formants[numVowels][numFormants] = {
        { 650.0f, 1080.0f, 2650.0f },   // A
        { 400.0f, 1700.0f, 2600.0f },   // E
        { 290.0f, 1870.0f, 2800.0f },   // I
        { 400.0f,  800.0f, 2600.0f },   // O
        { 350.0f,  600.0f, 2700.0f },   // U
    };
    static constexpr float gainsDb[numVowels][numFormants] = {
        { 0.0f,  -6.0f,  -7.0f },
        { 0.0f, -14.0f, -12.0f },
        { 0.0f, -15.0f, -18.0f },
        { 0.0f,  -9.0f, -12.0f },
        { 0.0f, -20.0f, -17.0f },
    };
    static constexpr float bandwidths[numFormants] = { 80.0f, 90.0f, 120.0f };
    static constexpr float formantMakeup = 1.6f;

    double sr = 44100.0;
    Parameters params;
    TptSvf sections[numFormants];
    double m0 = 1.0, m1 = 0.0, m2 = 0.0;
    float formantGain[numFormants] { 1.0f, 1.0f, 1.0f };
};

} // namespace fxme
