/*
  ------------------------------------------------------------------------------
    Reverb.h

    A stereo reverb: the Freeverb-derived WDL_ReverbEngine (WDL/verbengine.h,
    the same WDL submodule the convolution engine comes from) behind a float,
    block-oriented interface with the awkward parts taken care of.

    What the wrapper adds over the engine itself:

      - float in, float out. The engine works in double and its block form
        needs the input and output to be separate buffers, so the conversion
        buffers are allocated once in prepare().
      - Room size and damping only take effect after Reset(), which is easy to
        forget and re-tunes every comb when called. The wrapper tracks the two
        values and calls it only when one of them has actually moved.
      - Room size arrives as a plain 0 to 1 control and is mapped onto the
        range the engine is actually useful over (its own comment says
        "0.3..0.99 or so"); below that the tail is too short to be a reverb,
        above it never decays.

    Width follows the engine: 0 collapses the two channels to the same signal,
    1 leaves them fully apart, and negative values swap them.

    NOT part of the FxmeTools module umbrella (it needs the WDL headers):

        #include <FxmeTools/dsp/Reverb.h>

    verbengine.h is header-only, so nothing extra has to be compiled — the WDL
    submodule only has to be checked out.

    Threading: prepare() allocates — message thread / prepareToPlay only.
    Everything else is realtime safe.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <algorithm>
#include <cmath>
#include <vector>
#include "../../../WDL/WDL/verbengine.h"

namespace fxme
{

class Reverb
{
public:
    /** The window of the engine's room-size control that a 0 to 1 setting is
        spread over. */
    static constexpr float minRoomSize = 0.30f;
    static constexpr float maxRoomSize = 0.97f;

    /** Allocates the conversion buffers and sizes the engine for this rate.
        Message thread / prepareToPlay only. */
    void prepare (double sampleRate, int maxBlockSize)
    {
        blockSize = std::max (1, maxBlockSize);

        inL.assign  ((size_t) blockSize, 0.0);
        inR.assign  ((size_t) blockSize, 0.0);
        outL.assign ((size_t) blockSize, 0.0);
        outR.assign ((size_t) blockSize, 0.0);

        engine.SetSampleRate (sampleRate > 0.0 ? sampleRate : 48000.0);
        engine.SetRoomSize ((double) room);
        engine.SetDampening ((double) damp);
        engine.Reset (true);
    }

    /** Clears the tail without disturbing the settings. */
    void reset()
    {
        engine.Reset (true);
    }

    /** 0 to 1, mapped onto [minRoomSize, maxRoomSize]. Longer is bigger. */
    void setRoomSize (float roomSize01) noexcept
    {
        const float mapped = minRoomSize
            + std::clamp (roomSize01, 0.0f, 1.0f) * (maxRoomSize - minRoomSize);
        if (std::abs (mapped - room) < 1.0e-4f)
            return;

        room = mapped;
        engine.SetRoomSize ((double) room);
        retune = true;
    }

    /** 0 = bright tail, 1 = the high end dies first. */
    void setDamping (float damping01) noexcept
    {
        const float d = std::clamp (damping01, 0.0f, 1.0f);
        if (std::abs (d - damp) < 1.0e-4f)
            return;

        damp = d;
        engine.SetDampening ((double) damp);
        retune = true;
    }

    /** -1 to 1: 0 is mono, 1 is the full stereo spread, negative swaps the
        channels. Takes effect immediately, no retune needed. */
    void setWidth (float width) noexcept
    {
        engine.SetWidth ((double) std::clamp (width, -1.0f, 1.0f));
    }

    /** Renders the reverb of `in` into `out`. The output is the wet signal
        only; the caller mixes it. Input and output pointers may be the same.
        Blocks longer than the prepared size are processed in several passes,
        so no host can run past the conversion buffers. */
    void process (const float* srcL, const float* srcR,
                  float* dstL, float* dstR, int numSamples) noexcept
    {
        // Changing room size or damping only reaches the combs through Reset().
        // At a fixed sample rate it re-tunes them without allocating, so it is
        // safe here, but it is still done once per change rather than per block.
        if (retune)
        {
            engine.Reset (false);
            retune = false;
        }

        for (int start = 0; start < numSamples; start += blockSize)
        {
            const int n = std::min (blockSize, numSamples - start);

            for (int i = 0; i < n; ++i)
            {
                inL[(size_t) i] = (double) srcL[start + i];
                inR[(size_t) i] = (double) srcR[start + i];
            }

            engine.ProcessSampleBlock (inL.data(), inR.data(),
                                       outL.data(), outR.data(), n);

            for (int i = 0; i < n; ++i)
            {
                dstL[start + i] = (float) outL[(size_t) i];
                dstR[start + i] = (float) outR[(size_t) i];
            }
        }
    }

private:
    WDL_ReverbEngine engine;
    std::vector<double> inL, inR, outL, outR;
    int blockSize = 512;

    float room = 0.7f * (maxRoomSize - minRoomSize) + minRoomSize;
    float damp = 0.5f;
    bool  retune = false;
};

} // namespace fxme
