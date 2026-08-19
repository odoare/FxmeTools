/*
  ------------------------------------------------------------------------------
    StereoCrossDelay.h

    A stereo delay with independent left and right times and a full feedback
    matrix: each side feeds itself and, through the cross term, the other one.
    Equal times with only cross feedback gives the classic ping-pong; unequal
    times with a little of both gives the drifting, never-quite-repeating
    pattern a pair of independent delays cannot.

    Built on two fxme::DelayLine instances with their own feedback switched
    off, so the matrix can be applied from outside: the smoothed delay-time
    target (time changes glide instead of clicking) and the interpolated
    fractional read come along with them.

    Feedback is kept stable rather than trusted: the class scales the pair of
    gains reaching each line so their sum stays below one. Two 0 dB feedbacks
    plus a 0 dB cross term would otherwise be an oscillator, and nothing in a
    plugin's parameter ranges usually stops a user asking for exactly that.

    Threading: prepare() allocates — message thread / prepareToPlay only.
    Everything else is realtime safe. Header-only.

    Usage (one instance per delay voice):

        delay.prepare (sampleRate, 6.0f);
        delay.setDelaySeconds (0.25f, 0.375f);
        delay.setFeedback (0.5f, 0.5f, 0.2f);
        delay.process (inL, inR, outL, outR, numSamples);

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include "DelayLine.h"
#include <algorithm>
#include <cmath>

namespace fxme
{

class StereoCrossDelay
{
public:
    /** Largest total feedback allowed into one line. Below 1 the loop always
        decays; the headroom keeps a long tail from ringing forever. */
    static constexpr float maxTotalFeedback = 0.985f;

    /** Allocates both lines — message thread / prepareToPlay only. */
    void prepare (double sampleRateIn, float maxDelaySeconds)
    {
        sampleRate = sampleRateIn > 0.0 ? sampleRateIn : 48000.0;

        left.prepare (sampleRate, maxDelaySeconds);
        right.prepare (sampleRate, maxDelaySeconds);
        left.setFeedback (0.0f);     // the matrix below is the only feedback
        right.setFeedback (0.0f);

        setDamping (damping);
        reset();
    }

    void reset()
    {
        left.reset();
        right.reset();
        lastL = lastR = 0.0f;
        dampStateL = dampStateR = 0.0f;
    }

    void setDelaySeconds (float leftSeconds, float rightSeconds)
    {
        left.setDelaySeconds (leftSeconds);
        right.setDelaySeconds (rightSeconds);
    }

    /** Glide applied to a delay-time change (default is fxme::DelayLine's
        30 ms). Long values give the tape-style pitch slur. */
    void setTimeGlideSeconds (float seconds)
    {
        left.setSmoothingSeconds (seconds);
        right.setSmoothingSeconds (seconds);
    }

    /** Linear feedback gains: each side into itself, and each side into the
        other. All three are taken as magnitudes and then scaled together if
        either line would receive a total of maxTotalFeedback or more, so the
        balance the caller asked for is kept while the loop stays stable. */
    void setFeedback (float leftGain, float rightGain, float crossGain) noexcept
    {
        const float l = std::clamp (std::abs (leftGain),  0.0f, 1.0f);
        const float r = std::clamp (std::abs (rightGain), 0.0f, 1.0f);
        const float x = std::clamp (std::abs (crossGain), 0.0f, 1.0f);

        const float worst = std::max (l + x, r + x);
        const float scale = worst > maxTotalFeedback ? maxTotalFeedback / worst : 1.0f;

        fbL = l * scale;
        fbR = r * scale;
        fbX = x * scale;
    }

    /** 0 = the feedback path is transparent, 1 = strongly damped: a one-pole
        lowpass from about 20 kHz down to 200 Hz, so the repeats mellow as they
        decay. Applies to the fed-back signal only, never to the first pass. */
    void setDamping (float damp01) noexcept
    {
        damping = std::clamp (damp01, 0.0f, 1.0f);

        if (damping < 1.0e-4f)
        {
            dampCoef = 1.0f;   // exact pass-through
            return;
        }

        const double cutoff = 20000.0 * std::pow (0.01, (double) damping);
        dampCoef = 1.0f - (float) std::exp (-6.283185307179586 * cutoff / sampleRate);
    }

    void processSample (float inL, float inR, float& outL, float& outR) noexcept
    {
        // The previous output is what feeds back: one sample of extra loop
        // delay against thousands in the line itself, and it keeps the matrix
        // out of the lines' own write path.
        dampStateL += dampCoef * (lastL - dampStateL);
        dampStateR += dampCoef * (lastR - dampStateR);

        outL = left.processSample  (inL + fbL * dampStateL + fbX * dampStateR);
        outR = right.processSample (inR + fbR * dampStateR + fbX * dampStateL);

        lastL = outL;
        lastR = outR;
    }

    /** Block form. The output pointers may alias the input pointers. */
    void process (const float* inL, const float* inR,
                  float* outL, float* outR, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            float l = 0.0f, r = 0.0f;
            processSample (inL[i], inR[i], l, r);
            outL[i] = l;
            outR[i] = r;
        }
    }

private:
    DelayLine left, right;
    double sampleRate = 48000.0;

    float fbL = 0.0f, fbR = 0.0f, fbX = 0.0f;
    float lastL = 0.0f, lastR = 0.0f;
    float damping = 0.0f, dampCoef = 1.0f;
    float dampStateL = 0.0f, dampStateR = 0.0f;
};

} // namespace fxme
