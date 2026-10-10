/*
  ------------------------------------------------------------------------------
    synth/MipmappedBuffer.h

    A recording stored as an octave pyramid, for reading it at any speed
    without aliasing: level 0 is the recording, each next level is the one
    before low-passed (63-tap windowed sinc, cut at 0.23 of its rate) and
    decimated by two, down to a few dozen samples. Reading `speed` level-0
    samples per output sample, the reader picks the level where that speed is
    about one sample per sample (levelForSpeed) and interpolates there; the
    level is fractional and the two neighbouring levels are blended, so a
    speed that changes continuously (a glide, a range whose width moves)
    changes the sound continuously too.

    Positions are always in level-0 samples, whatever level is read, so a
    caller can move freely between levels. Each level carries guard samples
    (edge values repeated), and positions are clamped to the recording, so
    any position is safe to read.

    Built once (allocates, about twice the recording's size) on a non-audio
    thread; read-only afterwards, realtime safe, shareable between threads.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/synth/WavetableReader.h>
#include <cmath>
#include <vector>

namespace fxme
{

class MipmappedBuffer
{
public:
    using Interpolation = WavetableReader::Interpolation;

    static constexpr int maxLevels = 20;
    static constexpr int guard = 4;
    static constexpr int minLevelLength = 32;

    /** How far below the alias-free level the reader leans (as
        WavetableSet::levelBias): reading the level one below lets the top of
        the band reach about 1.4 x Nyquist, folding back above 0.29 fs. */
    static constexpr float levelBias = 0.5f;

    /** Builds the pyramid from `numChannels` channels of `numSamples`. */
    void build (const float* const* channels, int numChannels, int numSamples);

    int getNumChannels() const noexcept { return numChannels; }
    int getNumSamples() const noexcept  { return numSamples; }
    int getNumLevels() const noexcept   { return numLevels; }

    /** The (fractional) level to read at `speed` level-0 samples per output
        sample, clamped to the levels built. */
    float levelForSpeed (double speed) const noexcept
    {
        if (speed <= 1.0e-9)
            return 0.0f;
        const double l = std::log2 (speed) + (double) levelBias;
        if (l <= 0.0)
            return 0.0f;
        const double top = (double) (numLevels - 1);
        return (float) (l < top ? l : top);
    }

    /** Channel `ch` at level-0 position `pos`, read from integer level `level`. */
    float read (int ch, int level, double pos, Interpolation mode) const noexcept
    {
        const auto& lv = levels[(std::size_t) ch][(std::size_t) level];
        const double p = pos * levelScale[(std::size_t) level];
        const double hi = (double) (lv.length - 1);
        const double q = p < 0.0 ? 0.0 : (p > hi ? hi : p);
        const int i = (int) q;
        return WavetableReader::interpolate (lv.data.data() + guard, i, (float) (q - (double) i), mode);
    }

    /** As read(), blending the two levels around a fractional `level`. */
    float readBlended (int ch, float level, double pos, Interpolation mode) const noexcept
    {
        const int k = (int) level;
        const float t = level - (float) k;
        if (t < 0.02f || k + 1 >= numLevels)
            return read (ch, k, pos, mode);
        if (t > 0.98f)
            return read (ch, k + 1, pos, mode);
        const float a = read (ch, k, pos, mode);
        return a + (read (ch, k + 1, pos, mode) - a) * t;
    }

    /** Level-0 samples of channel `ch` (no guards), for analysis or display. */
    const float* getLevel0 (int ch) const noexcept { return levels[(std::size_t) ch][0].data.data() + guard; }

private:
    struct Level
    {
        std::vector<float> data;   // guard + length + guard
        int length = 0;
    };

    std::vector<std::vector<Level>> levels;   // [channel][level]
    std::vector<double> levelScale;           // 1 / 2^level
    int numChannels = 0, numSamples = 0, numLevels = 0;
};

//==============================================================================
/** Reading a range of a MipmappedBuffer as one cycle of a loop: the
    oscillator of a range wavetable synth, without precomputed tables.

    The range is [start, start + cycle + overlap): one cycle of `cycle`
    samples, and `overlap` samples after it that are crossfaded into its
    beginning (linear or equal power) so the loop wraps without a step. Every
    value is a double and may change on every sample: moving a range is a
    continuous scan, never a switch.

    With no overlap the wrap is a step in the waveform; wrapStep() gives its
    size and polyBlep() the band-limited correction to add around it. */
namespace RangeLoop
{
    struct Range
    {
        double start = 0.0;     // level-0 samples
        double cycle = 2048.0;  // samples per cycle
        double overlap = 0.0;   // samples, at most the cycle
        bool equalPower = true;
    };

    inline float read (const MipmappedBuffer& b, int ch, float level, const Range& r, double phase,
                       MipmappedBuffer::Interpolation mode) noexcept
    {
        const double pos = phase * r.cycle;
        float v = b.readBlended (ch, level, r.start + pos, mode);
        if (pos < r.overlap)
        {
            const float t = (float) (pos / r.overlap);
            float gIn, gOut;
            if (r.equalPower)
            {
                gIn  = std::sin (1.5707963f * t);
                gOut = std::cos (1.5707963f * t);
            }
            else
            {
                gIn = t;
                gOut = 1.0f - t;
            }
            v = gIn * v + gOut * b.readBlended (ch, level, r.start + r.cycle + pos, mode);
        }
        return v;
    }

    /** With no overlap: the value just after the wrap minus the value the
        signal would have reached without it. */
    inline float wrapStep (const MipmappedBuffer& b, int ch, float level, const Range& r,
                           MipmappedBuffer::Interpolation mode) noexcept
    {
        return b.readBlended (ch, level, r.start, mode) - b.readBlended (ch, level, r.start + r.cycle, mode);
    }

    /** Two-sample polyBLEP residual for a unit step at phase 0 of a phase
        `t` in [0, 1) advancing `dt` per sample; add 0.5 x step x residual. */
    inline float polyBlep (double t, double dt) noexcept
    {
        if (dt <= 0.0 || dt >= 0.5)
            return 0.0f;
        if (t < dt)
        {
            const double x = t / dt;
            return (float) (x + x - x * x - 1.0);
        }
        if (t > 1.0 - dt)
        {
            const double x = (t - 1.0) / dt;
            return (float) (x * x + x + x + 1.0);
        }
        return 0.0f;
    }
}

} // namespace fxme
