/*
  ------------------------------------------------------------------------------
    util/ProcessSpec.h

    Framework-free replacement for JUCE's dsp::ProcessSpec.

    Like AudioBufferView, this converts implicitly from any struct exposing the
    same three fields, so existing call sites that pass a JUCE dsp::ProcessSpec
    keep compiling unchanged while the core header stays JUCE-free:

        void prepare (fxme::ProcessSpec spec);      // core signature

        // a JUCE dsp::ProcessSpec value:
        auto js = makeJuceSpec (sr, blockSize, numCh);
        prepare (js);                               // still compiles

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <cstdint>
#include <type_traits>
#include <utility>

namespace fxme
{

namespace detail
{
    template <typename T, typename = void>
    struct isProcessSpecLike : std::false_type {};

    template <typename T>
    struct isProcessSpecLike<T, decltype (void (std::declval<const T&>().sampleRate),
                                          void (std::declval<const T&>().maximumBlockSize),
                                          void (std::declval<const T&>().numChannels))>
        : std::true_type {};
}

struct ProcessSpec
{
    double        sampleRate       = 0.0;
    std::uint32_t maximumBlockSize = 0;
    std::uint32_t numChannels      = 0;

    ProcessSpec() = default;

    ProcessSpec (double rate, std::uint32_t blockSize, std::uint32_t channels) noexcept
        : sampleRate (rate), maximumBlockSize (blockSize), numChannels (channels) {}

    /** Implicit conversion from JUCE's dsp::ProcessSpec (or anything shaped like it). */
    template <typename SpecType,
              typename = typename std::enable_if<
                  detail::isProcessSpecLike<SpecType>::value
                  && ! std::is_same<typename std::decay<SpecType>::type, ProcessSpec>::value>::type>
    ProcessSpec (const SpecType& other) noexcept
        : sampleRate       (static_cast<double> (other.sampleRate)),
          maximumBlockSize (static_cast<std::uint32_t> (other.maximumBlockSize)),
          numChannels      (static_cast<std::uint32_t> (other.numChannels)) {}
};

} // namespace fxme
