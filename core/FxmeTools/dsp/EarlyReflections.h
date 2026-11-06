/*
  ------------------------------------------------------------------------------
    EarlyReflections.h

    The early part of a reverb: a handful of discrete reflections before the
    diffuse tail, from a multi-tap delay. Eight taps, each with its time, its
    gain, its pan (equal power) and a one-pole low-pass that darkens the later
    taps more (each bounce off a wall loses some highs).

    Patterns (tap times in ms at Size 1, scaled by setTimeScale):

      room       eight reflections from 4 to 39 ms, irregular, alternating
                 sides: a medium room.
      hall       sparser and later, 10 to 79 ms, darker.
      plate      two faint taps (a plate has no early reflections to speak
                 of; they only take the edge off the onset).
      stairwell  a flutter echo: regular reflections every 8.5 ms between
                 two parallel hard walls, alternating sides and slowly
                 decaying, barely darkened (hard walls keep their highs). The
                 ringing a real stairwell has.

    getTaps() exposes a pattern for drawing it.

    Threading: prepare() allocates; the rest is realtime safe. Header-only.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/dsp/ModDelayLine.h>
#include <FxmeTools/dsp/Sanitise.h>
#include <algorithm>
#include <array>
#include <cmath>

namespace fxme
{

class EarlyReflections
{
public:
    enum class Pattern { room = 0, hall, plate, stairwell };

    static constexpr int numTaps = 8;
    static constexpr float maxTimeScale = 2.5f;

    struct TapSpec
    {
        float ms, gain, pan;        // pan -1 .. 1
        float lowpassHz;
    };
    using Taps = std::array<TapSpec, numTaps>;

    static Taps getTaps (Pattern p) noexcept
    {
        switch (p)
        {
            case Pattern::room:
                return {{ { 4.3f, 0.85f, -0.6f, 14000.0f }, { 7.9f, 0.75f, 0.5f, 12500.0f },
                          { 11.2f, 0.68f, -0.3f, 11000.0f }, { 15.7f, 0.60f, 0.8f, 9500.0f },
                          { 19.4f, 0.52f, -0.8f, 8500.0f }, { 24.6f, 0.45f, 0.2f, 7500.0f },
                          { 31.0f, 0.38f, -0.5f, 6500.0f }, { 38.5f, 0.30f, 0.6f, 5500.0f } }};
            case Pattern::hall:
                return {{ { 9.5f, 0.70f, -0.4f, 11000.0f }, { 17.2f, 0.62f, 0.6f, 9500.0f },
                          { 23.8f, 0.56f, -0.7f, 8500.0f }, { 33.1f, 0.50f, 0.3f, 7500.0f },
                          { 41.9f, 0.44f, -0.2f, 6500.0f }, { 52.6f, 0.38f, 0.8f, 5500.0f },
                          { 64.3f, 0.32f, -0.6f, 4800.0f }, { 79.0f, 0.26f, 0.4f, 4200.0f } }};
            case Pattern::plate:
                return {{ { 1.9f, 0.30f, -0.3f, 16000.0f }, { 3.1f, 0.25f, 0.3f, 16000.0f },
                          { 5.0f, 0.0f, 0.0f, 16000.0f }, { 6.0f, 0.0f, 0.0f, 16000.0f },
                          { 7.0f, 0.0f, 0.0f, 16000.0f }, { 8.0f, 0.0f, 0.0f, 16000.0f },
                          { 9.0f, 0.0f, 0.0f, 16000.0f }, { 10.0f, 0.0f, 0.0f, 16000.0f } }};
            case Pattern::stairwell:
            {
                Taps t {};
                for (int i = 0; i < numTaps; ++i)
                    t[(size_t) i] = { 8.5f * (float) (i + 1), 0.82f * std::pow (0.86f, (float) i),
                                      (i % 2 == 0 ? -0.7f : 0.7f), 16000.0f - 900.0f * (float) i };
                return t;
            }
        }
        return getTaps (Pattern::room);
    }

    //==========================================================================
    void prepare (double sampleRate)
    {
        fs = sampleRate > 0.0 ? sampleRate : 48000.0;
        line.prepare (fs, 0.080f * maxTimeScale + 0.01f);
        updateTaps();
        reset();
    }

    void reset()
    {
        line.reset();
        lowState.fill (0.0f);
    }

    void setPattern (Pattern p) { pattern = p; updateTaps(); }
    Pattern getPattern() const noexcept { return pattern; }

    /** Scales every tap time (0.25 .. maxTimeScale). */
    void setTimeScale (float s) { timeScale = std::clamp (s, 0.25f, maxTimeScale); updateTaps(); }

    void process (float inL, float inR, float& outL, float& outR) noexcept
    {
        line.write (sanitiseSample (0.5f * (inL + inR), 16.0f));
        float l = 0.0f, r = 0.0f;
        for (size_t i = 0; i < (size_t) numTaps; ++i)
        {
            if (gain[i] == 0.0f)
                continue;
            const float v = line.read (delay[i]);
            lowState[i] += lowCoef[i] * (v - lowState[i]);
            l += gainL[i] * lowState[i];
            r += gainR[i] * lowState[i];
        }
        outL = l;
        outR = r;
    }

private:
    void updateTaps() noexcept
    {
        const auto taps = getTaps (pattern);
        for (size_t i = 0; i < (size_t) numTaps; ++i)
        {
            const auto& t = taps[i];
            delay[i] = std::max (2.0f, (float) (t.ms * 0.001 * timeScale * fs));
            gain[i] = t.gain;
            const float angle = (t.pan + 1.0f) * 0.25f * 3.14159265f;   // equal power
            gainL[i] = t.gain * std::cos (angle);
            gainR[i] = t.gain * std::sin (angle);
            lowCoef[i] = 1.0f - (float) std::exp (-2.0 * 3.141592653589793
                                                   * std::min ((double) t.lowpassHz, 0.45 * fs) / fs);
        }
    }

    double fs = 48000.0;
    Pattern pattern = Pattern::room;
    float timeScale = 1.0f;
    ModDelayLine line;
    std::array<float, numTaps> delay {}, gain {}, gainL {}, gainR {}, lowCoef {}, lowState {};
};

} // namespace fxme
