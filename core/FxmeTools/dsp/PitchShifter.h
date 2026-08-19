/*
  ------------------------------------------------------------------------------
    PitchShifter.h

    Delay-line pitch shifter shared across FX-Mechanics projects. Header-only
    and JUCE-free — depends only on <cmath> / <vector> — so it can be reused
    from any C++ project.

    Classic dual-tap design: two read taps sweep a short circular delay line
    (the "window") at a rate of (1 - ratio) windows per window, half a window
    apart, with an equal-power sin/cos crossfade so each tap is silent exactly
    when its delay wraps. Power is preserved through the crossfade
    (sin^2 + cos^2 = 1), which suits the small shifts (a few tens of cents)
    used for take-doubling / chorus effects; larger shifts work but exhibit
    the usual granular artefacts of the technique.

    The pitch ratio is smoothed internally with a one-pole so it can be set
    from a control thread at any rate without zipper noise. Reads use 4-point
    Catmull-Rom interpolation.

    An optional constant base delay (setBaseDelay) is added to both taps —
    handy for chorus-type effects where each voice needs its own short delay
    on top of the detune. It is smoothed like the ratio, so moving it glides
    (with the natural doppler slur) instead of clicking.

    Typical use:

        fxme::PitchShifter shifter;
        shifter.prepare (sampleRate);          // allocates - not realtime-safe
        shifter.setWindow (50.0f);             // ms (optional, 50 by default)
        shifter.resetPhase (0.37f);            // decorrelate multiple voices

        // control thread (or top of the audio callback):
        shifter.setPitchCents (cents);         // e.g. -20 .. +20

        // audio thread:
        shifter.process (in, out, numSamples); // or per-sample processSample()

    Note: at ratio == 1 the taps stand still and the output is the sum of two
    static delays (a mild comb). Callers that mix the shifted signal at a
    level proportional to the shift amount (as spread/doubling effects do)
    mask this by construction.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <cmath>
#include <vector>

namespace fxme
{

class PitchShifter
{
public:
    // Allocates the delay line — call from prepareToPlay, never from the
    // audio thread. `maxWindowMs` / `maxBaseDelayMs` bound later setWindow()
    // and setBaseDelay() calls.
    void prepare (double newSampleRate, float maxWindowMs = 100.0f,
                  float maxBaseDelayMs = 0.0f)
    {
        sampleRate   = newSampleRate;
        maxWindowSamples = (float) (sampleRate * (double) maxWindowMs * 0.001);
        maxBaseSamples   = (float) (sampleRate * (double) maxBaseDelayMs * 0.001);

        std::size_t size = 16;
        while (size < (std::size_t) (maxWindowSamples + maxBaseSamples + 8.0f))
            size <<= 1;
        buffer.assign (size, 0.0f);
        mask = size - 1;

        setWindow (windowMs);
        setSmoothingTime (smoothingSeconds);
        reset();
    }

    // Crossfade window length in milliseconds (clamped to the prepared
    // maximum). Longer windows lower the sweep rate but smear transients.
    void setWindow (float ms)
    {
        windowMs = ms;
        if (! buffer.empty())
            windowSamples = std::fmin ((float) (sampleRate * (double) ms * 0.001),
                                       maxWindowSamples);
    }

    // Response time of the internal pitch-ratio smoothing.
    void setSmoothingTime (float seconds)
    {
        smoothingSeconds = seconds;
        smoothCoeff = 1.0f - std::exp (-1.0f / (float) (sampleRate * (double) std::fmax (1.0e-4f, seconds)));
    }

    // Constant extra delay on both taps (clamped to the prepared maximum).
    void setBaseDelay (float ms) noexcept
    {
        targetBaseDelay = std::fmin (std::fmax (0.0f, (float) (sampleRate * (double) ms * 0.001)),
                                     maxBaseSamples);
    }

    void setPitchRatio (float ratio) noexcept        { targetRatio = ratio; }
    void setPitchCents (float cents) noexcept        { targetRatio = std::exp2 (cents * (1.0f / 1200.0f)); }
    void setPitchSemitones (float semitones) noexcept { targetRatio = std::exp2 (semitones * (1.0f / 12.0f)); }

    // Clears the delay line and jumps the smoothed values to their targets.
    void reset()
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        writePos = 0;
        currentRatio = targetRatio;
        currentBaseDelay = targetBaseDelay;
    }

    // Sets the tap phase (0..1). Give each voice of a multi-voice effect a
    // different phase so their sweep cycles (and thus artefacts) decorrelate.
    void resetPhase (float phase01) noexcept
    {
        phase = phase01 - std::floor (phase01);
    }

    float processSample (float x) noexcept
    {
        buffer[(std::size_t) writePos & mask] = x;

        currentRatio     += smoothCoeff * (targetRatio     - currentRatio);
        currentBaseDelay += smoothCoeff * (targetBaseDelay - currentBaseDelay);

        phase += (1.0f - currentRatio) / windowSamples;
        phase -= std::floor (phase);

        const float p2 = (phase < 0.5f) ? phase + 0.5f : phase - 0.5f;

        // Equal-power crossfade: each tap is silent exactly at its wrap point.
        const float g1 = std::sin (3.14159265f * phase);
        const float g2 = std::sin (3.14159265f * p2);

        const float y = g1 * readInterpolated (2.0f + currentBaseDelay + phase * windowSamples)
                      + g2 * readInterpolated (2.0f + currentBaseDelay + p2    * windowSamples);

        // Wrapped, not free-running: the read arithmetic above depends on
        // writePos staying inside the buffer.
        writePos = (writePos + 1) & (long) mask;
        return y;
    }

    void process (const float* in, float* out, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
            out[i] = processSample (in[i]);
    }

private:
    // 4-point Catmull-Rom read at `delay` samples behind the write position.
    float readInterpolated (float delay) const noexcept
    {
        // The whole-sample and fractional parts are kept apart, and the
        // position arithmetic stays in integers: `writePos - delay` in float
        // would silently lose fractional resolution as writePos grows (a
        // float carries 24 significant bits, so past ~2^22 samples — under
        // two minutes at 48 kHz — the interpolation fraction quantises and
        // the sweeping taps start stepping instead of sliding, which sounds
        // like a slowly worsening aliasing/roughness).
        const auto  delayInt = (long) delay;              // delay is always >= 2
        const float frac     = 1.0f - (delay - (float) delayInt);
        const long  i1       = writePos - delayInt - 1 + (long) (mask + 1);

        const float y0 = buffer[(std::size_t) (i1 - 1) & mask];
        const float y1 = buffer[(std::size_t)  i1      & mask];
        const float y2 = buffer[(std::size_t) (i1 + 1) & mask];
        const float y3 = buffer[(std::size_t) (i1 + 2) & mask];

        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * frac + c2) * frac + c1) * frac + y1;
    }

    std::vector<float> buffer;
    std::size_t mask = 0;
    long writePos = 0;

    double sampleRate = 48000.0;
    float windowMs = 50.0f, windowSamples = 2400.0f, maxWindowSamples = 4800.0f;
    float maxBaseSamples = 0.0f;
    float phase = 0.0f;
    float targetRatio = 1.0f, currentRatio = 1.0f;
    float targetBaseDelay = 0.0f, currentBaseDelay = 0.0f;
    float smoothingSeconds = 0.05f, smoothCoeff = 0.01f;
};

} // namespace fxme
