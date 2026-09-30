/*
  ------------------------------------------------------------------------------
    FrequencyShifter.h

    A frequency shifter (the Bode kind), mono or stereo: every frequency of
    the input moves by the same number of Hz, up or down. Unlike a pitch
    shift, which multiplies frequencies and keeps harmonies in tune, this
    adds to them, so harmonics drift out of their ratios: a few Hz is a slow
    phasing, tens of Hz a beating, detuned shimmer, hundreds of Hz a
    metallic, bell-like transformation. Inside a delay's feedback loop each
    repeat moves again, so the repeats spiral up or down ("barber pole").

    The input is split into two signals 90 degrees apart (an analytic pair)
    by two chains of four second-order all-pass sections, the classic
    polyphase design by Olli Niemitalo, 90 degrees to within a fraction of a
    degree from about 20 Hz to 20 kHz at 44.1 or 48 kHz. Multiplied by a
    quadrature oscillator at the shift frequency and summed, they keep only
    one sideband: the input moved up (or down, for a negative shift), its
    mirror rejected. No latency; the all-passes shift phases, not levels.

    Stereo: both channels share the oscillator, so the image is kept.

    Threading: prepare() from the message thread; the rest is realtime safe
    (no allocation).

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/util/Math.h>
#include <cmath>

namespace fxme
{

class FrequencyShifter
{
public:
    static constexpr int maxChannels = 2;

    void prepare (double sampleRateIn, int numChannelsIn) noexcept
    {
        sampleRate  = sampleRateIn > 0.0 ? sampleRateIn : 48000.0;
        numChannels = fxme::jlimit (1, maxChannels, numChannelsIn);
        smoothCoef  = 1.0f - (float) std::exp (-1.0 / (0.02 * sampleRate));
        reset();
    }

    /** Clears the all-passes and restarts the oscillator; the shift jumps
        to its target. */
    void reset() noexcept
    {
        for (auto& c : channels)
            c = {};
        cosPhase = 1.0;
        sinPhase = 0.0;
        sampleCount = 0;
        currentHz = targetHz;
    }

    /** The shift in Hz: positive up, negative down. Glides over about
        20 ms. */
    void setShiftHz (float hz) noexcept         { targetHz = hz; }

    /** One frame: `frame[0 .. numChannels - 1]` in, shifted in place. */
    void processFrame (float* frame) noexcept
    {
        currentHz += smoothCoef * (targetHz - currentHz);

        const float c = (float) cosPhase, s = (float) sinPhase;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto& st = channels[ch];
            const float x = frame[ch];

            // Path a has the extra sample of delay; the two come out 90
            // degrees apart, b lagging a (b is a's Hilbert transform, with
            // the sign that makes a - j b the analytic signal).
            const float a = chain (st.a, coefsA, st.delayed);
            st.delayed = x;
            const float b = chain (st.b, coefsB, x);

            // Single sideband: the analytic signal (a - j b) times e^(j w t),
            // real part. Up for a positive shift, down for a negative one
            // (checked by the core test, which measures where a tone lands).
            frame[ch] = a * c + b * s;
        }

        advanceOscillator();
    }

    /** Mono convenience: numChannels must be 1. */
    float processSample (float x) noexcept
    {
        processFrame (&x);
        return x;
    }

private:
    /** One second-order all-pass: y[n] = a^2 (x[n] + y[n-2]) - x[n-2]. */
    struct Section
    {
        float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
    };

    struct Channel
    {
        Section a[4], b[4];
        float delayed = 0.0f;
    };

    static float chain (Section (&sections)[4], const float (&coefs)[4], float x) noexcept
    {
        for (int i = 0; i < 4; ++i)
        {
            auto& s = sections[i];
            const float y = coefs[i] * (x + s.y2) - s.x2;
            s.x2 = s.x1;
            s.x1 = x;
            s.y2 = s.y1;
            s.y1 = y;
            x = y;
        }
        return x;
    }

    /** Rotates the oscillator by one sample at the current shift, and
        brings it back onto the unit circle now and then (the rotation
        drifts in magnitude by rounding). */
    void advanceOscillator() noexcept
    {
        const double w = 2.0 * 3.141592653589793238 * (double) currentHz / sampleRate;
        const double cw = std::cos (w), sw = std::sin (w);
        const double c = cosPhase * cw - sinPhase * sw;
        const double s = sinPhase * cw + cosPhase * sw;
        cosPhase = c;
        sinPhase = s;

        if (++sampleCount >= 1024)
        {
            sampleCount = 0;
            const double norm = 1.0 / std::sqrt (cosPhase * cosPhase + sinPhase * sinPhase);
            cosPhase *= norm;
            sinPhase *= norm;
        }
    }

    // Niemitalo's coefficients, squared (the sections use a^2).
    static constexpr float coefsA[4] { 0.6923878f * 0.6923878f, 0.9360654322959f * 0.9360654322959f,
                                       0.9882295226860f * 0.9882295226860f, 0.9987488452737f * 0.9987488452737f };
    static constexpr float coefsB[4] { 0.4021921162426f * 0.4021921162426f, 0.8561710882420f * 0.8561710882420f,
                                       0.9722909545651f * 0.9722909545651f, 0.9952884791278f * 0.9952884791278f };

    double sampleRate = 48000.0;
    int numChannels = 1;
    Channel channels[maxChannels];

    float targetHz = 0.0f, currentHz = 0.0f, smoothCoef = 0.001f;
    double cosPhase = 1.0, sinPhase = 0.0;
    int sampleCount = 0;
};

} // namespace fxme
