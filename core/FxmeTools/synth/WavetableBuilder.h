/*
  ------------------------------------------------------------------------------
    synth/WavetableBuilder.h

    Turns a range of an audio buffer into a band-limited WavetableSet: the
    whole range becomes one cycle of the wave, whatever its length, so the
    pitch it plays at depends only on the oscillator's frequency.

    The steps, for each channel:

      1. Range. [centre - width / 2, centre + width / 2] of the source,
         clamped to the buffer.
      2. Snap (optional). ZeroCrossing moves both edges to the nearest rising
         zero crossing; Period sets the width to a whole number of the period
         detected around the centre (normalised autocorrelation, by FFT) and
         then puts the start on a rising zero crossing.
      3. Loop overlap. The last `overlap * width` samples are blended into the
         first ones (linear or equal-power), so the cycle wraps without a step.
         The cycle is then (1 - overlap) * width samples long.
      4. Resampling to a power-of-two work length M (2048 to 32768): cubic
         interpolation when the cycle is shorter than M, box averaging when it
         is longer. One forward real FFT of size M follows; on upsampling,
         only the harmonics the cycle really carries (below cycle / 2) are
         kept, which also removes the interpolator's images.
      5. Mipmaps. One inverse FFT of size 2048 per level, each keeping the
         harmonics that level allows (WavetableSet::maxHarmonic).

    DC is removed (a table with an offset would put a step on every note-on).
    Each set is phase-aligned (WavetableSet.h): rotated so its lowest strong
    harmonic starts as a sine at phase 0, the same rotation for both channels,
    so sets cut from nearby positions crossfade without cancelling.
    The set keeps the source's amplitude: a quiet range gives a quiet wave.

    Not realtime: it allocates its work buffers on first use and runs FFTs of
    up to 32768 points. Run it on a worker thread (see WavetableCache) or the
    message thread. One builder per thread.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/synth/WavetableSet.h>
#include <FxmeTools/util/Fft.h>
#include <complex>
#include <cstdint>
#include <memory>
#include <vector>

namespace fxme
{

//==============================================================================
/** Immutable audio the tables are cut from. Built on a non-audio thread and
    shared (std::shared_ptr<const WavetableSource>) between the threads that
    read it. */
struct WavetableSource
{
    std::vector<std::vector<float>> channels;   // 1 or 2, all numSamples long
    int numSamples = 0;
    double sampleRate = 44100.0;

    /** Bumped by the owner each time a new source replaces the old one, so a
        table cut from an older source can be told apart from a current one. */
    int generation = 0;

    int getNumChannels() const noexcept { return (int) channels.size(); }

    /** The mono mix at `i` (for analysis: zero crossings, period). */
    float mono (int i) const noexcept
    {
        if (channels.size() == 1)
            return channels[0][(std::size_t) i];
        return 0.5f * (channels[0][(std::size_t) i] + channels[1][(std::size_t) i]);
    }
};

//==============================================================================
struct WavetableBuildSettings
{
    enum class Snap : int { none = 0, zeroCrossing, period };
    enum class OverlapShape : int { linear = 0, equalPower };

    double centre = 0.0;      // in source samples
    double width  = 2048.0;   // in source samples
    float overlap = 0.0f;     // fraction of the width, 0 .. 0.5
    OverlapShape overlapShape = OverlapShape::equalPower;
    Snap snap = Snap::none;
};

//==============================================================================
class WavetableBuilder
{
public:
    static constexpr int minWorkOrder = WavetableSet::tableOrder;   // 2048
    static constexpr int maxWorkOrder = 15;                         // 32768
    static constexpr int minWidth = 8;

    WavetableBuilder();

    /** Builds `out` (which must be allocated) from `source`. Returns false,
        leaving a silent set, when the source is empty. */
    bool build (const WavetableSource& source, const WavetableBuildSettings& settings,
                WavetableSet& out);

    /** The range actually used by the last build(), after clamping and
        snapping, in source samples: [start, start + length). */
    int getLastStart() const noexcept  { return lastStart; }
    int getLastLength() const noexcept { return lastLength; }

    //==========================================================================
    // Analysis helpers, usable on their own (a GUI drawing snap points).

    /** The rising zero crossing of the mono mix nearest to `position`, within
        `radius` samples; `position` itself when there is none. */
    static int nearestRisingZeroCrossing (const WavetableSource& source, int position, int radius) noexcept;

    /** The period, in samples, detected around `centre` by normalised
        autocorrelation over lags [minLag, maxLag]; 0 when no clear period is
        found. Allocates (use off the audio thread). */
    double detectPeriod (const WavetableSource& source, int centre, int minLag = 16, int maxLag = 4096);

private:
    void resolveRange (const WavetableSource& source, const WavetableBuildSettings& s,
                       int& start, int& length);
    void makeCycle (const float* src, int sourceLength, int start, int length, int overlapSamples,
                    WavetableBuildSettings::OverlapShape shape);
    void resampleCycle (int workSize);
    void buildLevels (int workOrder, int harmonicLimit, float* const* tables, bool computeAlignment);
    double alignTurns = 0.0;    // rotation applied to every channel of the set being built

    std::vector<float> cycle;               // the looped cycle, length cycleLength
    int cycleLength = 0;

    std::vector<float> work;                // 2 * M floats for RealFft
    std::vector<float> levelBuf;            // 2 * tableSize floats
    std::vector<std::unique_ptr<RealFft>> workFfts;   // index = order - minWorkOrder
    RealFft tableFft { WavetableSet::tableOrder };

    std::unique_ptr<RealFft> acfFft;        // for detectPeriod
    std::vector<float> acfBuf;

    int lastStart = 0, lastLength = 0;
};

} // namespace fxme
