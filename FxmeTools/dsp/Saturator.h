/*
  ------------------------------------------------------------------------------
    Saturator.h

    Mono tube-style saturation stage built on the shared waveshaper curves
    (dsp/Waveshapers.h): drive -> curve -> power-supply "sag" rail model ->
    DC blocker. This is the distortion core of FxmeFX's Tube without the tone
    shelf and without any parameter/JUCE coupling, so it can be dropped into
    any per-channel chain. Use one instance per channel.

    Models
      Standard : static tanh(drive*x + bias) — the reference curve.
      Dynamic  : same curve, but clipping into a sagging supply rail.
      Triode   : asymmetric 12AX7-ish curve into the sagging rail.
      ClassAB  : push-pull pair of half-wave saturators; `bias` sets the
                 crossover overlap instead of an offset.

    The rail model: the virtual supply voltage droops with signal demand
    (fast ~10 ms attack) and recovers slowly (~200 ms), so hard-driven
    passages get both harder clipping and less swing — dynamic "give".

    Threading: prepare() from the message thread (no allocation, but not
    glitch-free); everything else is realtime-safe. Header-only, <cmath> only.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <cmath>
#include "Waveshapers.h"

namespace fxme
{

class Saturator
{
public:
    enum class Model { Standard = 0, Dynamic, Triode, ClassAB };

    void prepare (double sampleRate)
    {
        // Power-supply sag time constants: fast attack, slow recovery.
        sagAttackCoef  = std::exp (-1.0f / (0.010f * (float) sampleRate)); // ~10 ms
        sagReleaseCoef = std::exp (-1.0f / (0.200f * (float) sampleRate)); // ~200 ms
        reset();
    }

    void reset()
    {
        railVoltage = 1.0f;
        x1 = y1 = 0.0f;
    }

    void setModel (Model m)      { model = m; }
    /** Drive in dB (0..40 is the usual range). */
    void setDriveDb (float dB)   { drive = std::pow (10.0f, dB * 0.05f); }
    /** Operating-point offset (0..0.5). For ClassAB this is the crossover
        overlap instead. */
    void setBias (float b)       { bias = b; }
    /** Sag amount, 0 (stiff supply) .. 1 (maximum droop). Ignored by the
        Standard model. */
    void setSag (float s)        { sag = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s); }

    float processSample (float x)
    {
        // RC rail model: rail droops with current draw, recovers slowly.
        if (sag > 0.0f && model != Model::Standard)
        {
            const float driveAbs = drive * std::fabs (x);
            // Saturating demand in [0, 1] so very high drive doesn't blow up
            // the target.
            const float demand = driveAbs / (1.0f + driveAbs);
            const float target = 1.0f - sag * demand;
            const float coef   = (target < railVoltage) ? sagAttackCoef : sagReleaseCoef;
            railVoltage = coef * railVoltage + (1.0f - coef) * target;
            railVoltage = railVoltage < 0.1f ? 0.1f : (railVoltage > 1.0f ? 1.0f : railVoltage);
        }
        else
        {
            railVoltage = 1.0f;
        }

        // The input is normalised to rail-relative units, the curve clips to
        // ±1 in that frame, then is scaled back: both "harder clipping" and
        // "less swing" as the rail collapses.
        const float effRail = railVoltage;
        const float invRail = 1.0f / effRail;
        const float driven  = x * drive;

        float saturated;
        switch (model)
        {
            case Model::Standard: saturated = std::tanh (driven + bias); break;
            case Model::Dynamic:  saturated = effRail * std::tanh ((driven + bias) * invRail); break;
            case Model::Triode:   saturated = effRail * shapers::triodeCurve ((driven + bias) * invRail); break;
            case Model::ClassAB:  saturated = effRail * shapers::classAbCurve (driven * invRail, bias); break;
            default:              saturated = std::tanh (driven + bias); break;
        }

        // DC blocker (removes offset from asymmetry / bias).
        const float out = saturated - x1 + 0.995f * y1;
        x1 = saturated;
        y1 = out;
        return out;
    }

private:
    Model model = Model::Standard;
    float drive = 1.0f, bias = 0.0f, sag = 0.0f;
    float railVoltage = 1.0f;
    float sagAttackCoef = 0.0f, sagReleaseCoef = 0.0f;
    float x1 = 0.0f, y1 = 0.0f;
};

} // namespace fxme
