/*
  ------------------------------------------------------------------------------
    util/AudioBuffer.h

    Owning multi-channel audio buffer — the counterpart to util/AudioBufferView.h.

    The view deliberately owns nothing, which makes it right for parameters and
    useless for members: a DSP class that allocates its own scratch or output
    storage needs something that holds the samples. This is that, and no more
    than that.

    It exposes the same accessors JUCE's AudioBuffer does, which is not just
    familiarity — it is what makes an instance satisfy the shape test in
    AudioBufferView.h, so it converts implicitly to AudioBufferView and
    ConstAudioBufferView with no adapter and no conversion operator:

        fxme::AudioBuffer storage;
        storage.setSize (2, 512);
        someCoreFunction (storage);          // takes an AudioBufferView

    Channels are stored in one contiguous block with a separate array of channel
    pointers, so getArrayOfWritePointers() hands out exactly the layout the view
    and JUCE both expect.

    setSize() allocates and is for prepare-time only. Everything else — clear(),
    the accessors — allocates nothing and is safe on the audio thread.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/util/AudioBufferView.h>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace fxme
{

template <typename SampleType>
class BasicAudioBuffer
{
public:
    BasicAudioBuffer() = default;

    BasicAudioBuffer (int numChannels, int numSamples)
    {
        setSize (numChannels, numSamples);
    }

    //==============================================================================
    /** Resizes and zeroes the buffer. Allocates: prepare-time only.

        Unlike JUCE's, this always clears and never preserves the old contents —
        every caller in this library discards them anyway, and a flag nobody
        passes is a flag that eventually gets passed by mistake. */
    void setSize (int numChannels, int numSamples)
    {
        nChannels = numChannels > 0 ? numChannels : 0;
        nSamples  = numSamples  > 0 ? numSamples  : 0;

        storage.assign (static_cast<std::size_t> (nChannels) * static_cast<std::size_t> (nSamples),
                        SampleType());
        pointers.resize (static_cast<std::size_t> (nChannels));

        for (int c = 0; c < nChannels; ++c)
            pointers[static_cast<std::size_t> (c)]
                = storage.data() + static_cast<std::size_t> (c) * static_cast<std::size_t> (nSamples);
    }

    void clear() noexcept
    {
        std::fill (storage.begin(), storage.end(), SampleType());
    }

    void clearChannel (int channel) noexcept
    {
        auto* d = pointers[static_cast<std::size_t> (channel)];
        std::fill (d, d + nSamples, SampleType());
    }

    //==============================================================================
    int getNumChannels() const noexcept { return nChannels; }
    int getNumSamples()  const noexcept { return nSamples; }

    /** These two are what AudioBufferView.h's shape test looks for, so an
        instance converts implicitly to either view. */
    SampleType* const* getArrayOfWritePointers() noexcept             { return pointers.data(); }
    const SampleType* const* getArrayOfReadPointers() const noexcept { return pointers.data(); }

    SampleType*       getWritePointer (int channel) noexcept       { return pointers[static_cast<std::size_t> (channel)]; }
    const SampleType* getReadPointer  (int channel) const noexcept { return pointers[static_cast<std::size_t> (channel)]; }

    SampleType getSample (int channel, int index) const noexcept
    {
        return pointers[static_cast<std::size_t> (channel)][index];
    }

    void setSample (int channel, int index, SampleType value) noexcept
    {
        pointers[static_cast<std::size_t> (channel)][index] = value;
    }

private:
    std::vector<SampleType>  storage;
    std::vector<SampleType*> pointers;
    int nChannels = 0;
    int nSamples  = 0;
};

using AudioBuffer  = BasicAudioBuffer<float>;
using AudioBufferD = BasicAudioBuffer<double>;

} // namespace fxme
