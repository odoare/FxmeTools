/*
  ------------------------------------------------------------------------------
    GrainLooper.h

    Grain recorder / looper shared across FX-Mechanics projects. Header-only
    and JUCE-free — depends only on <cmath> / <vector> / <algorithm> — so it
    can be reused from any C++ project.

    trigger() records the next `grainSeconds` of the incoming signal, then
    loops that grain endlessly. Loop playback keeps the energy of the grain
    train (almost) constant: the grain plays *unwindowed* except for short
    half-sine fades at its edges (setCrossfade, 30 ms by default, clamped to
    half the grain), and consecutive instances overlap by exactly the fade
    length, fade-out cos against fade-in sin, so the summed power through
    every seam is sin^2 + cos^2 = 1 and the content is at unity gain for the
    rest of the grain — no audible per-grain pumping.

    setPlayFraction (1 by default) shortens what each instance *plays* of the
    grain without changing the instance rate: below 1 the loop becomes a
    train of short grains separated by silence, at the same period as the
    seamless loop. Faded edges are kept (clamped to half the played part).

    While no grain is playing the input passes through unchanged, and every
    transition is glitch-free by construction:

      * live -> grain and grain -> live (stop()) blend through a short
        equal-power crossfade;
      * re-triggering records into the spare buffer while the old grain keeps
        playing, and the loop adopts the new grain at the next instance start
        (which begins at zero envelope) — no click, no interruption.

    Typical use:

        fxme::GrainLooper looper;
        looper.prepare (sampleRate, 2.0f);      // allocates - not realtime-safe

        // UI thread -> audio thread via your own flag / message:
        looper.trigger (0.25f);                 // realtime-safe
        looper.stop();

        // audio thread:
        out[i] = looper.processSample (in[i]);  // == in[i] while inactive

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace fxme
{

class GrainLooper
{
public:
    // Allocates the two grain buffers — call from prepareToPlay, never from
    // the audio thread. `maxGrainSeconds` bounds later trigger() lengths.
    void prepare (double newSampleRate, float maxGrainSeconds = 2.0f)
    {
        sampleRate = newSampleRate;
        maxLength  = std::max (64, (int) (sampleRate * (double) maxGrainSeconds));
        for (auto& b : buffers)
            b.assign ((size_t) maxLength, 0.0f);

        mixCoeff = 1.0f - std::exp (-1.0f / (float) (sampleRate * 0.015));
        playing = recording = false;
        mix = 0.0f;
        for (auto& v : voices)
            v.pos = -1;
    }

    // Length of the half-sine edge fades (and thus of the instance overlap),
    // clamped to half the grain at play time.
    void setCrossfade (float seconds) noexcept
    {
        fadeSeconds = std::clamp (seconds, 1.0e-3f, 0.5f);
    }

    // Fraction of the grain each instance plays (instance rate unchanged);
    // below 1 the loop leaves silence between consecutive grains. Applies
    // from the next instance start.
    void setPlayFraction (float fraction) noexcept
    {
        playFraction = std::clamp (fraction, 0.0f, 1.0f);
    }

    // Starts recording a new grain of the given length (clamped to the
    // prepared maximum). Realtime-safe. If a grain is already looping it
    // keeps playing until the new one is ready.
    void trigger (float grainSeconds)
    {
        recordLength   = std::clamp ((int) (sampleRate * (double) grainSeconds), 64, maxLength);
        recordBuffer   = playing ? 1 - currentBuffer : 0;
        recordPosition = 0;
        recording      = true;
    }

    // Back to normal mode: the grain fades out, the live input fades back in.
    void stop()
    {
        playing   = false;
        recording = false;
    }

    bool isLooping() const noexcept { return playing; }

    float processSample (float x)
    {
        // Idle fast path: not looping, fully faded back to live, not
        // recording — pure pass-through (cheap enough to leave many
        // instances permanently in a processing chain).
        if (! playing && ! recording && mix <= 1.0e-4f)
        {
            mix = 0.0f;
            for (auto& v : voices)
                v.pos = -1;
            return x;
        }

        if (recording)
        {
            buffers[(size_t) recordBuffer][(size_t) recordPosition] = x;
            if (++recordPosition >= recordLength)
            {
                recording     = false;
                currentBuffer = recordBuffer;
                currentLength = recordLength;
                currentFade   = std::clamp ((int) (sampleRate * (double) fadeSeconds),
                                            1, currentLength / 2);
                if (! playing)
                {
                    playing = true;
                    samplesToNextStart = 0;      // first instance starts now
                    nextVoice = 0;
                    for (auto& v : voices)
                        v.pos = -1;
                }
            }
        }

        mix += mixCoeff * ((playing ? 1.0f : 0.0f) - mix);

        // Render the loop while audible (also during the stop() fade-out).
        float grain = 0.0f;
        if (playing || mix > 1.0e-4f)
        {
            // Start the next instance when the current one reaches its
            // fade-out, so instances overlap by exactly the fade length. New
            // instances always adopt the newest grain (at zero envelope).
            if (playing && --samplesToNextStart <= 0)
            {
                auto& v = voices[(size_t) nextVoice];
                v.buffer     = currentBuffer;
                v.playLength = std::max (32, (int) (playFraction * (float) currentLength));
                v.fade       = std::min (currentFade, v.playLength / 2);
                v.pos        = 0;
                // The instance period is set by the full grain regardless of
                // how much of it is played, so setPlayFraction leaves the
                // grain rate untouched (silence fills the difference).
                samplesToNextStart = std::max (1, currentLength - currentFade);
                nextVoice = 1 - nextVoice;
            }

            for (auto& v : voices)
            {
                if (v.pos < 0)
                    continue;

                // Half-sine edges, unity sustain in between: through a seam
                // the outgoing cos and the incoming sin are power-complementary.
                const int sustainEnd = v.playLength - v.fade;
                float env = 1.0f;
                if (v.pos < v.fade)
                    env = std::sin (1.5707963f * ((float) v.pos + 0.5f) / (float) v.fade);
                else if (v.pos >= sustainEnd)
                    env = std::cos (1.5707963f * ((float) (v.pos - sustainEnd) + 0.5f) / (float) v.fade);

                grain += env * buffers[(size_t) v.buffer][(size_t) v.pos];

                if (++v.pos >= v.playLength)
                    v.pos = -1;
            }
        }

        // Equal-power blend between the live input and the grain loop.
        const float a = mix * 1.57079633f;
        return std::cos (a) * x + std::sin (a) * grain;
    }

    void process (const float* in, float* out, int numSamples)
    {
        for (int i = 0; i < numSamples; ++i)
            out[i] = processSample (in[i]);
    }

private:
    struct Voice
    {
        int buffer = 0, playLength = 64, fade = 32;
        int pos = -1;                  // sample position; < 0 = idle
    };

    std::vector<float> buffers[2];
    Voice voices[2];

    double sampleRate = 48000.0;
    int maxLength = 96000;
    int currentBuffer = 0, currentLength = 64, currentFade = 32;
    int recordBuffer = 0, recordLength = 64, recordPosition = 0;
    int samplesToNextStart = 0, nextVoice = 0;
    bool playing = false, recording = false;
    float mix = 0.0f, mixCoeff = 0.01f;
    float fadeSeconds = 0.03f;
    float playFraction = 1.0f;
};

} // namespace fxme
