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

        float* const* channels = buffer.getArrayOfWritePointers();
        fxme::ImpulseEnergy::normalise (channels, buffer.getNumChannels(),
                                        buffer.getNumSamples());

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
} // namespace ImpulseEnergy
} // namespace fxme
