/*
  ------------------------------------------------------------------------------
    VideoFileSource.h

    FrameSource that decodes a video file with FFmpeg on a background thread.

    JUCE offers no cross-platform way to grab frames from a video file
    (juce::VideoComponent is a player widget, Windows/macOS only), so FFmpeg
    is the portable route. Frames are paced in wall time from their
    presentation timestamps, the file loops at EOF (optional), and playback
    can be paused/resumed — the clock shifts so resuming stays in tempo.

    Only compiled when the consumer defines FXME_HAS_FFMPEG=1 and links
    libavformat / libavcodec / libavutil / libswscale (see
    fxmetools_attach_video() in cmake/FxmeTools.cmake, which finds them with
    pkg-config). Without it the class still exists — start() simply returns
    false — so consuming code and UIs need no #ifdefs.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#ifndef FXME_HAS_FFMPEG
 #define FXME_HAS_FFMPEG 0
#endif

#include "FrameSource.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <memory>

namespace fxme
{

/**
 * @class VideoFileSource
 * @brief Video-file decoding (FFmpeg) with PTS pacing, looping and transport.
 */
class VideoFileSource : public FrameSource,
                        private juce::Thread
{
public:
    explicit VideoFileSource (const juce::File& fileToLoad);
    ~VideoFileSource() override;

    /** Longest side of the emitted frames; larger material is scaled down
        during colour conversion (cheaper than decoding 4K to full size).
        Call before start(). 0 disables the cap. */
    void setMaxOutputDimension (int maxDimension)  { maxOutputDimension = juce::jmax (0, maxDimension); }

    /** Opens the demuxer/decoder synchronously (so failure is reported here)
        and starts the decode thread. Returns false without FFmpeg support. */
    bool start() override;
    void stop() override;

    juce::String getName() const override { return file.getFileName(); }
    bool isLive() const override          { return true; }

    juce::File getFile() const            { return file; }

    //==========================================================================
    // Transport — no-ops when FFmpeg support is compiled out.

    void setPaused (bool shouldPause)     { paused.store (shouldPause); }
    bool isPaused() const                 { return paused.load(); }

    /** Rewind to the start at EOF (default) instead of stopping. */
    void setLooping (bool shouldLoop)     { looping.store (shouldLoop); }
    bool isLooping() const                { return looping.load(); }

    /** Presentation time of the most recent frame, in seconds. */
    double getPositionSeconds() const     { return positionSeconds.load(); }

    /** Stream duration in seconds, or 0 when unknown / not open. */
    double getDurationSeconds() const     { return durationSeconds.load(); }

private:
    void run() override;

    struct Impl;                    // hides the FFmpeg types from this header
    std::unique_ptr<Impl> impl;

    juce::File file;
    int maxOutputDimension = 800;
    std::atomic<bool> paused { false };
    std::atomic<bool> looping { true };
    std::atomic<double> positionSeconds { 0.0 };
    std::atomic<double> durationSeconds { 0.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VideoFileSource)
};

} // namespace fxme
