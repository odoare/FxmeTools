/*
  ------------------------------------------------------------------------------
    FormantFilter.h

    Vowel formant filter: a parallel bank of two band-pass biquads tuned to
    the first two formants (F1/F2) of a vowel, with linear interpolation
    between two vowels for "vowel glide" sweeps (first written for
    Gloubiboulga, generalised here for sharing).

    Vowel table (approximate male-voice formants, Hz):
        A = 800/1200, E = 400/2200, I = 300/2500, O = 500/1000, U = 300/800
    Bandwidths are fixed at 80 Hz (F1) and 100 Hz (F2).

    Usage: one instance per channel. Call setVowelBlend() at control rate
    (e.g. every 32 samples) — it recomputes both biquad coefficient sets —
    then processSample() per sample. Header-only, <cmath> only; all methods
    after prepare() are realtime-safe.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <cmath>
#include "Biquad.h"

namespace fxme
{

class FormantFilter
{
public:
    enum class Vowel { A = 0, E, I, O, U };
    static constexpr int kNumVowels   = 5;
    static constexpr int kNumFormants = 2;

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        reset();
        setVowelBlend (Vowel::A, Vowel::A, 0.0f);
    }

    void reset()
    {
        for (auto& f : filters)
            f.reset();
    }

    /** Tune the bank to the linear interpolation between two vowels,
        t in [0,1]: 0 = `from`, 1 = `to`. */
    void setVowelBlend (Vowel from, Vowel to, float t)
    {
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        for (int i = 0; i < kNumFormants; ++i)
        {
            const float f0 = formantHz[(int) from][i];
            const float f1 = formantHz[(int) to][i];
            const float f  = f0 + (f1 - f0) * t;
            // fxme::Biquad's bandpass takes Q; Q = centre / bandwidth.
            filters[i].c = BiquadCoeffs::bandpass (sr, f, f / bandwidthHz[i]);
        }
    }

    float processSample (float x)
    {
        // Parallel sum of the formant bands. The constant-skirt bandpass
        // attenuates a lot at these Qs; make up roughly for it.
        float out = 0.0f;
        for (auto& f : filters)
            out += f.processSample (x);
        return out * makeupGain;
    }

private:
    static constexpr float formantHz[kNumVowels][kNumFormants] = {
        { 800.0f, 1200.0f },   // A
        { 400.0f, 2200.0f },   // E
        { 300.0f, 2500.0f },   // I
        { 500.0f, 1000.0f },   // O
        { 300.0f,  800.0f },   // U
    };
    static constexpr float bandwidthHz[kNumFormants] = { 80.0f, 100.0f };
    static constexpr float makeupGain = 2.0f;

    double sr = 44100.0;
    Biquad filters[kNumFormants];
};

} // namespace fxme
