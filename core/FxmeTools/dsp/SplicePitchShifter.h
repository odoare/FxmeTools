/*
  ------------------------------------------------------------------------------
    SplicePitchShifter.h

    A time-domain pitch shifter for musical shifts (an octave or two either
    way), mono or stereo, whose splices are aligned on the signal.

    Like any delay-line shifter it reads a circular buffer faster (up) or
    slower (down) than it is written, and when the read head drifts out of
    its range it jumps by about one window and crossfades from the old head
    to the new one. A free-running shifter (fxme::PitchShifter) jumps by
    exactly one window, so on a tone each splice lands at an arbitrary phase:
    the output of a sine becomes a comb of lines around the target, with the
    target itself sometimes almost cancelled. Fine for a few cents of detune,
    not for an octave.

    Here each jump is placed, within a search range around one window, where
    the new head's recent history best matches the old head's (normalised
    cross-correlation, a coarse search then a fine one), so a periodic signal
    carries on in phase across the splice. On noise and dense mixes the
    alignment matters less and it behaves like the free-running kind.

    Stereo: both channels share one head position and one set of splices,
    decided on their sum, so the image does not wobble.

    At a ratio of 1 the head stands still: no splices, and the output is the
    input delayed by the head's current delay. getLatencySamples() is the
    average delay, half a window plus the minimum distance kept from the
    write position.

    Threading: prepare() allocates (message thread / prepareToPlay); the rest
    is realtime safe (no allocation, no locks). The splice search runs inside
    processFrame(), a few tens of thousands of multiply-adds once per splice
    (a splice every window / |ratio - 1| samples).

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/util/Math.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace fxme
{

class SplicePitchShifter
{
public:
    static constexpr int maxChannels = 2;

    /** Allocates. `windowMs` is the nominal jump between splices: longer is
        smoother on sustained tones, shorter follows transients better and
        lowers the latency. */
    void prepare (double sampleRateIn, int numChannelsIn, float windowMs = 40.0f)
    {
        sampleRate  = sampleRateIn > 0.0 ? sampleRateIn : 48000.0;
        numChannels = fxme::jlimit (1, maxChannels, numChannelsIn);

        window    = (float) (sampleRate * (double) fxme::jmax (5.0f, windowMs) * 0.001);
        crossfade = fxme::jmax (16, (int) (sampleRate * 0.010));    // 10 ms
        search    = fxme::jmax (8,  (int) (sampleRate * 0.008));    // +-8 ms
        corrLen   = fxme::jmax (16, (int) (sampleRate * 0.010));    // 10 ms of history

        // The head never comes closer to the write position than this (room
        // for the interpolation), and never further than a window, a search
        // range and a crossfade's worth of travel at the steepest ratio.
        minDelay = 4.0f;
        const float maxTravel = (float) crossfade * (maxRatio - 1.0f);
        const auto needed = (std::size_t) (minDelay + window + (float) search + maxTravel
                                           + (float) corrLen + (float) search + 16.0f);
        std::size_t size = 64;
        while (size < needed * 2)
            size <<= 1;
        mask = size - 1;

        for (auto& b : buffers)
            b.assign (numChannels > 0 ? size : 0, 0.0f);

        smoothCoef = 1.0f - (float) std::exp (-1.0 / (0.02 * sampleRate));
        reset();
    }

    /** Clears the buffer and puts the head back at its resting delay. */
    void reset()
    {
        for (int c = 0; c < numChannels; ++c)
            std::fill (buffers[(size_t) c].begin(), buffers[(size_t) c].end(), 0.0f);
        writePos = 0;
        currentRatio = targetRatio;
        headDelay = restingDelay();
        fading = false;
        fadePos = 0;
    }

    /** The shift, as a frequency ratio (clamped to 1/4 .. 4) or in
        semitones (-24 .. +24). Glides over about 20 ms. */
    void setPitchRatio (float ratio) noexcept
    {
        targetRatio = fxme::jlimit (1.0f / maxRatio, maxRatio, ratio);
    }

    void setPitchSemitones (float semitones) noexcept
    {
        setPitchRatio (std::exp2 (semitones * (1.0f / 12.0f)));
    }

    /** Average delay the shifter adds, in samples. */
    float getLatencySamples() const noexcept     { return minDelay + 0.5f * window; }

    /** One frame: `frame[0 .. numChannels - 1]` in, shifted in place. */
    void processFrame (float* frame) noexcept
    {
        for (int c = 0; c < numChannels; ++c)
            buffers[(size_t) c][(size_t) writePos & mask] = frame[c];

        currentRatio += smoothCoef * (targetRatio - currentRatio);

        // Time for a splice: the head is about to leave its range, allowing
        // for the distance it still travels while it fades out.
        if (! fading)
        {
            const float travel = (float) crossfade * std::abs (currentRatio - 1.0f);
            if (currentRatio > 1.0f && headDelay <= minDelay + travel)
                startSplice (headDelay + window);
            else if (currentRatio < 1.0f && headDelay >= minDelay + window + (float) search)
                startSplice (headDelay - window);
        }

        for (int c = 0; c < numChannels; ++c)
        {
            float y = read (c, headDelay);
            if (fading)
            {
                // Raised-cosine: the two heads are aligned, so their gains sum
                // to one rather than their powers.
                const float t = (float) fadePos / (float) crossfade;
                const float in = 0.5f - 0.5f * std::cos (fxme::MathConstants<float>::pi * t);
                y = in * y + (1.0f - in) * read (c, oldDelay);
            }
            frame[c] = y;
        }

        // Both heads advance at the ratio while the write position advances
        // by one, so their delays change by 1 - ratio.
        const float drift = 1.0f - currentRatio;
        headDelay += drift;
        if (fading)
        {
            oldDelay += drift;
            if (++fadePos >= crossfade)
                fading = false;
        }

        writePos = (writePos + 1) & (long) mask;
    }

    /** Mono convenience: `numChannels` must be 1. */
    float processSample (float x) noexcept
    {
        processFrame (&x);
        return x;
    }

private:
    static constexpr float maxRatio = 4.0f;

    /** Where the head sits between splices when the ratio is 1, and where it
        starts: the middle of its range, so a shift either way has room. */
    float restingDelay() const noexcept         { return minDelay + 0.5f * window; }

    /** Starts a crossfade from the current head to a new one near
        `nominalDelay`, placed where their recent histories match best. */
    void startSplice (float nominalDelay) noexcept
    {
        const float lo = minDelay + (float) crossfade * fxme::jmax (0.0f, currentRatio - 1.0f);
        const float hi = minDelay + window + (float) search;
        const float centre = fxme::jlimit (lo + (float) search, hi, nominalDelay);

        oldDelay = headDelay;
        headDelay = fxme::jmax (lo, centre + (float) bestOffset (headDelay, centre));
        fading = true;
        fadePos = 0;
    }

    /** The offset within +-search of `centre` whose history best matches the
        history behind `current`: a coarse pass every 4 samples on every 4th
        sample, then a fine pass around the winner. */
    int bestOffset (float current, float centre) const noexcept
    {
        const long ref  = writePos - (long) std::lround (current);
        const long base = writePos - (long) std::lround (centre);

        const auto score = [&] (int offset, int stride) noexcept
        {
            double cross = 0.0, energy = 1.0e-12;
            const long cand = base - offset;
            for (int k = 1; k <= corrLen; k += stride)
            {
                const float a = sum (ref - k), b = sum (cand - k);
                cross  += (double) a * b;
                energy += (double) b * b;
            }
            return cross / std::sqrt (energy);
        };

        int best = 0;
        double bestScore = -1.0e30;
        for (int o = -search; o <= search; o += 4)
        {
            const double s = score (o, 4);
            if (s > bestScore) { bestScore = s; best = o; }
        }

        const int coarse = best;
        bestScore = -1.0e30;
        for (int o = coarse - 3; o <= coarse + 3; ++o)
        {
            const double s = score (o, 1);
            if (s > bestScore) { bestScore = s; best = o; }
        }
        return best;
    }

    /** Both channels summed at buffer position `pos` (wrapped). */
    float sum (long pos) const noexcept
    {
        const auto i = (std::size_t) pos & mask;
        float s = buffers[0][i];
        if (numChannels > 1)
            s += buffers[1][i];
        return s;
    }

    /** Channel `c` at `delay` samples behind the write position, 4-point
        Catmull-Rom. */
    float read (int c, float delay) const noexcept
    {
        const auto& b = buffers[(size_t) c];
        const auto  whole = (long) delay;
        const float frac  = 1.0f - (delay - (float) whole);
        const long  i1    = writePos - whole - 1 + (long) (mask + 1);

        const float y0 = b[(std::size_t) (i1 - 1) & mask];
        const float y1 = b[(std::size_t)  i1      & mask];
        const float y2 = b[(std::size_t) (i1 + 1) & mask];
        const float y3 = b[(std::size_t) (i1 + 2) & mask];

        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * frac + c2) * frac + c1) * frac + y1;
    }

    std::vector<float> buffers[maxChannels];
    std::size_t mask = 0;
    long writePos = 0;

    double sampleRate = 48000.0;
    int numChannels = 1;
    float window = 1920.0f, minDelay = 4.0f;
    int crossfade = 480, search = 384, corrLen = 480;

    float targetRatio = 1.0f, currentRatio = 1.0f, smoothCoef = 0.001f;
    float headDelay = 964.0f, oldDelay = 0.0f;
    bool  fading = false;
    int   fadePos = 0;
};

} // namespace fxme
