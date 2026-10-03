/*
  ------------------------------------------------------------------------------
    ImpulseEnergy.h

    Loudness normalisation for convolution impulse responses: scales an IR so
    that every IR, short cabinet or long hall, comes out of a convolution at
    a comparable level.

    The measure is the IR's energy, the sum of its squared samples, averaged
    over its channels. Convolving with an IR multiplies the power of a
    broadband input (noise, and roughly music) by that energy, so an IR
    scaled to unit energy leaves such an input at the same RMS level: a long,
    dense reverb IR (huge energy, hence the very loud wet signal) and a short
    cabinet IR end up equally loud. Peaks and the spectrum's shape are left
    to the IR: only its overall gain changes.

    One gain for all channels, so a stereo IR keeps its balance between left
    and right. An IR with (next to) no energy is left alone.

    Two measures:

      - normalise(): flat energy, every hertz weighted alike: what white
        noise sees. Simple, but music is not white: most of its energy is in
        the lows and mids, and a reverb IR that boosts those (a forest's
        60 to 400 Hz, +10 to +19 dB) still comes out far louder than the dry
        signal, by 5 to 11 dB on pink noise, while its flat energy says 1.
      - normaliseLoudness(): pink-weighted energy, every octave weighted
        alike: what pink noise (and, roughly, music) sees. Measured with ten
        octave bands (31.5 Hz to 16 kHz), each band's energy compared with a
        flat IR's in the same band, then averaged; so it does not depend on
        the sample rate. This is the one to use for loudness: the six
        reverb IRs of FxmeFX come out within 0.4 dB of the input with it.

        float* const* channels = buffer.getArrayOfWritePointers();
        fxme::ImpulseEnergy::normaliseLoudness (channels, buffer.getNumChannels(),
                                                buffer.getNumSamples(), sampleRate);

    Not realtime: meant for when an IR is loaded (it reads every sample).

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <cmath>

namespace fxme
{
namespace ImpulseEnergy
{
    /** Below this mean energy an IR counts as silent and is not scaled
        (it would only amplify noise or rounding). */
    inline constexpr double silentEnergy = 1.0e-12;

    /** The sum of the squared samples of each channel, averaged over the
        channels. 0 for no channels or no samples. */
    inline double meanChannelEnergy (const float* const* channels, int numChannels, int numSamples) noexcept
    {
        if (channels == nullptr || numChannels <= 0 || numSamples <= 0)
            return 0.0;

        double total = 0.0;
        for (int c = 0; c < numChannels; ++c)
            if (const float* x = channels[c])
                for (int i = 0; i < numSamples; ++i)
                    total += (double) x[i] * (double) x[i];

        return total / (double) numChannels;
    }

    /** The gain that brings an IR of mean channel energy `energy` to
        `targetEnergy`; 1 for a silent IR. */
    inline float gainFor (double energy, double targetEnergy = 1.0) noexcept
    {
        if (! (energy > silentEnergy) || ! (targetEnergy > 0.0))
            return 1.0f;
        return (float) std::sqrt (targetEnergy / energy);
    }

    /** Scales the IR in place to `targetEnergy` (mean per channel; 1 by
        default: unit energy) with one gain for every channel. Returns the
        gain applied (1 when the IR is silent and left alone). */
    inline float normalise (float* const* channels, int numChannels, int numSamples,
                            double targetEnergy = 1.0) noexcept
    {
        const float gain = gainFor (meanChannelEnergy (channels, numChannels, numSamples), targetEnergy);
        if (gain == 1.0f)
            return gain;

        for (int c = 0; c < numChannels; ++c)
            if (float* x = channels[c])
                for (int i = 0; i < numSamples; ++i)
                    x[i] *= gain;

        return gain;
    }

    //==========================================================================
    /** Octave-band centres for the pink-weighted measure. Bands above 0.45
        times the sample rate are left out. */
    inline constexpr double octaveCentres[] { 31.5, 63.0, 125.0, 250.0, 500.0,
                                              1000.0, 2000.0, 4000.0, 8000.0, 16000.0 };

    namespace detail
    {
        /** Energy of `x` (a unit impulse when null) through an RBJ band-pass
            (0 dB peak), one octave wide, its ringing included: the filter
            runs on for `tail` samples of silence after the input ends (a
            short IR would otherwise lose most of a low band's energy). */
        inline double octaveBandEnergy (const float* x, int numSamples, int tail,
                                        double centre, double sampleRate)
        {
            constexpr double q = 1.41421356237;   // one octave
            const double w = 2.0 * 3.14159265358979323846 * centre / sampleRate;
            const double alpha = std::sin (w) / (2.0 * q);
            const double a0 = 1.0 + alpha;
            const double b0 = alpha / a0, b2 = -alpha / a0;
            const double a1 = -2.0 * std::cos (w) / a0, a2 = (1.0 - alpha) / a0;

            double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0, energy = 0.0;
            const int total = numSamples + tail;
            for (int i = 0; i < total; ++i)
            {
                const double in = i >= numSamples ? 0.0
                                : x != nullptr    ? (double) x[i]
                                                  : (i == 0 ? 1.0 : 0.0);
                const double y = b0 * in + b2 * x2 - a1 * y1 - a2 * y2;
                x2 = x1; x1 = in; y2 = y1; y1 = y;
                energy += y * y;
            }
            return energy;
        }
    }

    /** The pink-weighted energy of an IR: the mean over octave bands (and
        over channels) of its energy in the band divided by a unit impulse's
        energy in the same band. 1 for a unit impulse at any sample rate; the
        power gain the IR gives pink noise. Reads every sample ten times. */
    inline double pinkWeightedEnergy (const float* const* channels, int numChannels, int numSamples,
                                      double sampleRate)
    {
        if (channels == nullptr || numChannels <= 0 || numSamples <= 0 || ! (sampleRate > 0.0))
            return 0.0;

        // Long enough for every band filter to have died away after its
        // input stops (the lowest band rings for well under a second).
        const int tail = (int) (sampleRate * 1.0);

        double sum = 0.0;
        int bands = 0;
        for (double centre : octaveCentres)
        {
            if (centre > 0.45 * sampleRate)
                continue;

            const double reference = detail::octaveBandEnergy (nullptr, 1, tail, centre, sampleRate);
            double energy = 0.0;
            for (int c = 0; c < numChannels; ++c)
                if (channels[c] != nullptr)
                    energy += detail::octaveBandEnergy (channels[c], numSamples, tail, centre, sampleRate);
            sum += energy / (double) numChannels / reference;
            ++bands;
        }

        return bands > 0 ? sum / (double) bands : 0.0;
    }

    /** Scales the IR in place so that pink noise comes out at `targetEnergy`
        times its power (1 by default: as loud as it went in), with one gain
        for every channel. Returns the gain applied (1 when silent). Not
        realtime. */
    inline float normaliseLoudness (float* const* channels, int numChannels, int numSamples,
                                    double sampleRate, double targetEnergy = 1.0)
    {
        const float gain = gainFor (pinkWeightedEnergy (channels, numChannels, numSamples, sampleRate),
                                    targetEnergy);
        if (gain == 1.0f)
            return gain;

        for (int c = 0; c < numChannels; ++c)
            if (float* x = channels[c])
                for (int i = 0; i < numSamples; ++i)
                    x[i] *= gain;

        return gain;
    }
} // namespace ImpulseEnergy
} // namespace fxme
