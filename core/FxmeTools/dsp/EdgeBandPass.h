/*
  ------------------------------------------------------------------------------
    EdgeBandPass.h

    A band-pass given by its two edges, for keeping a signal inside a frequency
    range that moves: a 4th-order Butterworth high-pass at the lower edge and a
    4th-order Butterworth low-pass at the upper one (two biquads each, 24 dB per
    octave), mono.

    Meant for a band that has already been cut out spectrally and then
    processed in the time domain (saturated, crushed, downsampled...), which
    spreads energy outside it: this puts it back in its range. The slopes are
    far softer than a spectral mask, so a little of the out-of-band content
    remains near the edges, and a band narrower than about an octave loses
    some level inside it too (the two slopes overlap). That is the price of a
    time-domain filter; it is not compensated.

    The edges are smoothed (logarithmically, 20 ms by default) so dragging one
    does not zipper, and the coefficients are recomputed once per process()
    call, only when an edge has actually moved. An edge at the end of the
    audible range drops its stage entirely (no high-pass at or below 20 Hz, no
    low-pass at or above 20 kHz or near Nyquist): with both dropped the filter
    passes the signal through untouched.

    Threading: prepare() from the message thread / prepareToPlay; everything
    else is realtime safe (no allocation at all). Header-only.

    Usage:

        band.prepare (sampleRate);
        band.setEdges (200.0f, 2000.0f, true);   // snap on the first call
        ...
        band.setEdges (low, high);               // every block, smoothed
        band.process (samples, numSamples);

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/dsp/Biquad.h>
#include <algorithm>
#include <cmath>

namespace fxme
{

class EdgeBandPass
{
public:
    static constexpr float minEdgeHz = 20.0f;
    static constexpr float maxEdgeHz = 20000.0f;

    void prepare (double sampleRateIn, float smoothingSeconds = 0.02f) noexcept
    {
        sampleRate = sampleRateIn > 0.0 ? sampleRateIn : 48000.0;
        smoothing  = std::max (0.0f, smoothingSeconds);
        reset();
    }

    /** Clears the filters' state; the edges jump to their targets. */
    void reset() noexcept
    {
        for (auto& f : highPass) f.reset();
        for (auto& f : lowPass)  f.reset();
        currentLow  = targetLow;
        currentHigh = targetHigh;
        updateCoefficients();
    }

    /** New edges in Hz (either order). Smoothed towards unless `snap`, which
        is what the first call after prepare() or reset() usually wants. */
    void setEdges (float lowHz, float highHz, bool snap = false) noexcept
    {
        targetLow  = std::max (1.0f, std::min (lowHz, highHz));
        targetHigh = std::max (1.0f, std::max (lowHz, highHz));

        if (snap)
        {
            currentLow  = targetLow;
            currentHigh = targetHigh;
            updateCoefficients();
        }
    }

    /** Filters `x` in place. */
    void process (float* x, int numSamples) noexcept
    {
        if (numSamples <= 0)
            return;

        glideEdges (numSamples);

        if (highPassOn)
            for (auto& f : highPass)
                f.processBlock (x, numSamples);

        if (lowPassOn)
            for (auto& f : lowPass)
                f.processBlock (x, numSamples);
    }

    bool isHighPassActive() const noexcept { return highPassOn; }
    bool isLowPassActive()  const noexcept { return lowPassOn; }

private:
    // Q of the two sections of a 4th-order Butterworth: 1 / (2 cos (pi/8)) and
    // 1 / (2 cos (3 pi/8)).
    static constexpr float q1 = 0.54119610f;
    static constexpr float q2 = 1.30656296f;

    /** Moves the edges towards their targets by one call's worth of
        smoothing, in log frequency, and refreshes the coefficients if they
        moved enough to matter. */
    void glideEdges (int numSamples) noexcept
    {
        if (currentLow == targetLow && currentHigh == targetHigh)
            return;

        const float keep = smoothing > 0.0f
            ? (float) std::exp (-(double) numSamples / ((double) smoothing * sampleRate))
            : 0.0f;

        const auto glide = [keep] (float current, float target)
        {
            const float next = target * std::pow (current / target, keep);
            // Close enough: land exactly, so the coefficients stop updating.
            return std::abs (next / target - 1.0f) < 1.0e-4f ? target : next;
        };

        currentLow  = glide (currentLow,  targetLow);
        currentHigh = glide (currentHigh, targetHigh);
        updateCoefficients();
    }

    void updateCoefficients() noexcept
    {
        const float nyquistLimit = (float) (0.45 * sampleRate);
        const bool wasHighPassOn = highPassOn, wasLowPassOn = lowPassOn;

        highPassOn = currentLow > minEdgeHz;
        lowPassOn  = currentHigh < std::min (maxEdgeHz, nyquistLimit);

        // A stage coming back starts from rest rather than from whatever state
        // it was left in when it was dropped.
        if (highPassOn && ! wasHighPassOn)
            for (auto& f : highPass) f.reset();
        if (lowPassOn && ! wasLowPassOn)
            for (auto& f : lowPass) f.reset();

        if (highPassOn)
        {
            highPass[0].c = BiquadCoeffs::highpass (sampleRate, currentLow, q1);
            highPass[1].c = BiquadCoeffs::highpass (sampleRate, currentLow, q2);
        }

        if (lowPassOn)
        {
            lowPass[0].c = BiquadCoeffs::lowpass (sampleRate, currentHigh, q1);
            lowPass[1].c = BiquadCoeffs::lowpass (sampleRate, currentHigh, q2);
        }
    }

    double sampleRate = 48000.0;
    float smoothing = 0.02f;

    float targetLow = minEdgeHz, targetHigh = maxEdgeHz;
    float currentLow = minEdgeHz, currentHigh = maxEdgeHz;

    Biquad highPass[2], lowPass[2];
    bool highPassOn = false, lowPassOn = false;
};

} // namespace fxme
