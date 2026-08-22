/*
  ------------------------------------------------------------------------------
    FrameSource.h

    Interface for anything that produces a stream of images: a still picture,
    a webcam (V4L2 on Linux, juce::CameraDevice on Windows/macOS) or a video
    file decoded with FFmpeg.

    A source is a dumb producer: it pushes juce::Images through onFrame,
    possibly from its own capture/decode thread. All processing (colour
    adjustments, rate limiting, downscaling) belongs to the consumer, which
    is normally fxme::VideoEngine.

    Contract for implementors:
      * set onFrame before start(); never call it after stop() returns;
      * emit freshly created, immutable juce::Images (the consumer only
        copies the ref-counted handle);
      * start() returns false if the device/file could not be opened;
      * stop() must be safe to call when never started, and from the
        destructor.

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_graphics/juce_graphics.h>
#include <functional>

namespace fxme
{

/**
 * @class FrameSource
 * @brief Abstract producer of a juce::Image stream (still, camera or video file).
 */
class FrameSource
{
public:
    virtual ~FrameSource() = default;

    /** Begins producing frames. Returns false if the source could not open. */
    virtual bool start() = 0;

    /** Stops production; safe to call twice or without a prior start(). */
    virtual void stop() = 0;

    /** Human-readable description (file name, camera card name, ...). */
    virtual juce::String getName() const = 0;

    /** True for continuous streams (camera, video file), false for one-shot
        sources such as a still image. */
    virtual bool isLive() const = 0;

    /** Called with each new frame, possibly from a background thread.
        Set it before start(). */
    std::function<void (const juce::Image&)> onFrame;
};

} // namespace fxme
