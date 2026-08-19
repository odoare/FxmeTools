/*
  ------------------------------------------------------------------------------
    util/AudioBufferView.h

    Non-owning view over a multi-channel block of audio — the framework-free
    replacement for JUCE's AudioBuffer<float> at every core API boundary.

    The point of this class is that it converts *implicitly* from any buffer
    type exposing the JUCE-shaped accessors (getArrayOfWritePointers,
    getNumChannels, getNumSamples). That means a JUCE AudioBuffer<float> can
    still be passed straight to a core function with no call-site change, while
    the core header itself has no JUCE dependency whatsoever. The same trick
    will work for an iPlug2 (sample**, nChans, nFrames) triple, so core code does
    not need touching again if the plugin framework changes.

        void process (fxme::AudioBufferView buf);   // core signature

        // a JUCE AudioBuffer<float> value:
        auto jb = makeJuceBuffer (2, 512);
        process (jb);                               // still compiles

    Views do not own anything and do not outlive the buffer they point at.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <cstddef>
#include <cstring>
#include <type_traits>
#include <utility>

namespace fxme
{

namespace detail
{
    template <typename T, typename = void>
    struct isJuceLikeBuffer : std::false_type {};

    template <typename T>
    struct isJuceLikeBuffer<T, decltype (void (std::declval<T&>().getArrayOfWritePointers()),
                                         void (std::declval<const T&>().getNumChannels()),
                                         void (std::declval<const T&>().getNumSamples()))>
        : std::true_type {};

    template <typename T, typename = void>
    struct isJuceLikeConstBuffer : std::false_type {};

    template <typename T>
    struct isJuceLikeConstBuffer<T, decltype (void (std::declval<const T&>().getArrayOfReadPointers()),
                                              void (std::declval<const T&>().getNumChannels()),
                                              void (std::declval<const T&>().getNumSamples()))>
        : std::true_type {};
}

//==============================================================================
/** Read/write view over interleaved-by-channel audio (channel-pointer layout). */
template <typename SampleType>
class BasicAudioBufferView
{
public:
    BasicAudioBufferView() = default;

    BasicAudioBufferView (SampleType* const* channelPointers, int numChannels, int numSamples) noexcept
        : channels (channelPointers), nChannels (numChannels), nSamples (numSamples) {}

    /** Implicit conversion from any JUCE-shaped buffer (JUCE's AudioBuffer<SampleType>, ...). */
    template <typename BufferType,
              typename = typename std::enable_if<detail::isJuceLikeBuffer<BufferType>::value>::type>
    BasicAudioBufferView (BufferType& buffer) noexcept
        : channels  (buffer.getArrayOfWritePointers()),
          nChannels (buffer.getNumChannels()),
          nSamples  (buffer.getNumSamples()) {}

    int getNumChannels() const noexcept { return nChannels; }
    int getNumSamples()  const noexcept { return nSamples; }
    bool isEmpty()       const noexcept { return channels == nullptr || nChannels <= 0 || nSamples <= 0; }

    SampleType*       getChannel (int c)       noexcept { return channels[c]; }
    const SampleType* getChannel (int c) const noexcept { return channels[c]; }

    SampleType*       operator[] (int c)       noexcept { return channels[c]; }
    const SampleType* operator[] (int c) const noexcept { return channels[c]; }

    SampleType* const* getArrayOfWritePointers() const noexcept { return channels; }

    SampleType  getSample (int c, int i) const noexcept          { return channels[c][i]; }
    void        setSample (int c, int i, SampleType v) noexcept  { channels[c][i] = v; }
    void        addSample (int c, int i, SampleType v) noexcept  { channels[c][i] += v; }

    void clear() noexcept
    {
        for (int c = 0; c < nChannels; ++c)
            std::memset (channels[c], 0, sizeof (SampleType) * static_cast<std::size_t> (nSamples));
    }

    void clearChannel (int c) noexcept
    {
        std::memset (channels[c], 0, sizeof (SampleType) * static_cast<std::size_t> (nSamples));
    }

    void applyGain (SampleType gain) noexcept
    {
        for (int c = 0; c < nChannels; ++c)
        {
            auto* d = channels[c];
            for (int i = 0; i < nSamples; ++i)
                d[i] *= gain;
        }
    }

    void applyGain (int channel, SampleType gain) noexcept
    {
        auto* d = channels[channel];
        for (int i = 0; i < nSamples; ++i)
            d[i] *= gain;
    }

    /** Copies from another view, channel by channel, up to the common size. */
    void copyFrom (const BasicAudioBufferView& source) noexcept
    {
        const int c = nChannels < source.nChannels ? nChannels : source.nChannels;
        const int n = nSamples  < source.nSamples  ? nSamples  : source.nSamples;

        for (int i = 0; i < c; ++i)
            std::memcpy (channels[i], source.channels[i], sizeof (SampleType) * static_cast<std::size_t> (n));
    }

private:
    SampleType* const* channels = nullptr;
    int nChannels = 0;
    int nSamples  = 0;
};

//==============================================================================
/** Read-only view. Converts from JUCE-shaped buffers via getArrayOfReadPointers. */
template <typename SampleType>
class BasicConstAudioBufferView
{
public:
    BasicConstAudioBufferView() = default;

    BasicConstAudioBufferView (const SampleType* const* channelPointers, int numChannels, int numSamples) noexcept
        : channels (channelPointers), nChannels (numChannels), nSamples (numSamples) {}

    template <typename BufferType,
              typename = typename std::enable_if<detail::isJuceLikeConstBuffer<BufferType>::value>::type>
    BasicConstAudioBufferView (const BufferType& buffer) noexcept
        : channels  (buffer.getArrayOfReadPointers()),
          nChannels (buffer.getNumChannels()),
          nSamples  (buffer.getNumSamples()) {}

    BasicConstAudioBufferView (const BasicAudioBufferView<SampleType>& v) noexcept
        : channels (v.getArrayOfWritePointers()), nChannels (v.getNumChannels()), nSamples (v.getNumSamples()) {}

    int getNumChannels() const noexcept { return nChannels; }
    int getNumSamples()  const noexcept { return nSamples; }
    bool isEmpty()       const noexcept { return channels == nullptr || nChannels <= 0 || nSamples <= 0; }

    const SampleType* getChannel (int c) const noexcept  { return channels[c]; }
    const SampleType* operator[] (int c) const noexcept  { return channels[c]; }
    SampleType getSample (int c, int i) const noexcept   { return channels[c][i]; }

    const SampleType* const* getArrayOfReadPointers() const noexcept { return channels; }

private:
    const SampleType* const* channels = nullptr;
    int nChannels = 0;
    int nSamples  = 0;
};

using AudioBufferView       = BasicAudioBufferView<float>;
using ConstAudioBufferView  = BasicConstAudioBufferView<float>;
using AudioBufferViewD      = BasicAudioBufferView<double>;
using ConstAudioBufferViewD = BasicConstAudioBufferView<double>;

} // namespace fxme
