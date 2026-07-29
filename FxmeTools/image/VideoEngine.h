/*
  ------------------------------------------------------------------------------
    VideoEngine.h

    Turn-key image input for a plugin: owns one fxme::FrameSource (still
    picture, webcam or video file), pumps it at a chosen refresh rate,
    applies brightness / contrast / saturation / mirroring, and publishes the
    result to the GUI and to any processing code.

        capture / decode thread ──▶ latest raw frame (mailbox, short lock)
        message thread (Timer)  ──▶ adjustments ──▶ ProcessedFrame
                                     ├─ getImage() + ChangeBroadcaster (GUI)
                                     └─ onFrame callback (analysis, DSP, ...)

    The timer only reprocesses when a new frame arrived or a setting changed,
    so a still image costs nothing in the steady state.

    Minimal use:

        fxme::VideoEngine engine;
        engine.setUpdateRateHz (30);
        engine.onFrame = [this] (const fxme::VideoEngine::ProcessedFrame& f)
                         { myAnalysis.process (f.working); };
        engine.addChangeListener (this);          // repaint the display
        engine.openCamera (fxme::VideoEngine::getCameraDeviceIds()[0]);

    Backends are selected for you: V4L2 on Linux, juce::CameraDevice on
    Windows/macOS (needs FXME_HAS_JUCE_CAMERA + juce_video), FFmpeg for video
    files (needs FXME_HAS_FFMPEG). See fxmetools_attach_video() in
    cmake/FxmeTools.cmake — it finds the libraries and sets those defines.
    Everything degrades gracefully: without a backend the matching open()
    returns false and the rest keeps working.

    Threading: all public methods are message-thread only, except the
    setters marked "any thread" (they only write atomics, so a plugin's audio
    thread may push modulated adjustment values every block).

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include "FrameSource.h"
#include "ImageAdjustments.h"
#include <juce_events/juce_events.h>
#include <atomic>
#include <memory>

namespace fxme
{

/**
 * @class VideoEngine
 * @brief Owns an image/video/camera source and publishes processed frames.
 */
class VideoEngine : public juce::ChangeBroadcaster,
                    private juce::Timer
{
public:
    /** What the active source is (also what persists in plugin state). */
    enum class SourceType { none = 0, stillImage, camera, videoFile, custom };

    /** One processed frame, handed to onFrame listeners. */
    struct ProcessedFrame
    {
        juce::Image image;     ///< adjustments applied, capture resolution (display)
        juce::Image working;   ///< same content capped to workingMaxDimension;
                               ///< identical to `image` when no cap applies
    };

    VideoEngine();
    ~VideoEngine() override;

    //==========================================================================
    /** @name Source management (message thread) */
    ///@{

    /** Loads a picture from disk. */
    bool loadImageFile (const juce::File& file);

    /** Uses an image already in memory (generated pattern, BinaryData asset). */
    bool loadImage (const juce::Image& image, const juce::String& name);

    /** Opens a video file (requires FFmpeg support). */
    bool loadVideoFile (const juce::File& file);

    /** Opens a webcam. `deviceId` comes from getCameraDeviceIds():
        "/dev/videoN" on Linux, the device index as a string elsewhere. */
    bool openCamera (const juce::String& deviceId);

    /** Installs any custom FrameSource (NDI, screen capture, ...); the engine
        takes ownership and starts it. Reported as SourceType::custom. */
    bool setSource (std::unique_ptr<FrameSource> newSource,
                    SourceType type = SourceType::custom);

    void closeSource();

    /** Available webcams, in matching order (names for the UI, ids to open). */
    static juce::StringArray getCameraDeviceNames();
    static juce::StringArray getCameraDeviceIds();

    /** True when the build can open webcams / video files at all. */
    static bool isCameraSupported();
    static bool isVideoFileSupported();
    ///@}

    //==========================================================================
    /** @name Video-file transport (no-ops for other source types) */
    ///@{
    void setVideoPaused (bool shouldPause);
    bool isVideoPaused() const;
    void setVideoLooping (bool shouldLoop);
    double getVideoPositionSeconds() const;
    double getVideoDurationSeconds() const;
    ///@}

    //==========================================================================
    /** @name Live settings (any thread — atomic stores only) */
    ///@{

    /** Refresh rate of the processing timer, 1..120 Hz. */
    void setUpdateRateHz (int hz)                 { rateHz.store (juce::jlimit (1, 120, hz)); }
    int getUpdateRateHz() const                   { return rateHz.load(); }

    void setAdjustments (const ImageAdjustments& a);
    ImageAdjustments getAdjustments() const;

    /** Longest side of ProcessedFrame::working. Analysis and DSP usually want
        a small image; 0 (default) means "same as the display image". */
    void setWorkingMaxDimension (int maxDimension) { workingMax.store (juce::jmax (0, maxDimension)); }
    int getWorkingMaxDimension() const             { return workingMax.load(); }

    /** Forces the next timer tick to reprocess even if nothing changed. */
    void refresh()                                 { forceRefresh.store (true); }
    ///@}

    //==========================================================================
    /** @name Outputs (message thread) */
    ///@{

    /** Last processed frame at capture resolution — what a display should draw. */
    juce::Image getImage() const                  { return processed.image; }

    /** Last processed frame capped to the working size (see setWorkingMaxDimension). */
    juce::Image getWorkingImage() const           { return processed.working; }

    /** Called on the message thread with each newly processed frame, just
        before the change broadcast. */
    std::function<void (const ProcessedFrame&)> onFrame;

    bool hasSource() const                        { return source != nullptr; }
    SourceType getSourceType() const              { return sourceType; }
    juce::File getSourceFile() const              { return sourceFile; }
    juce::String getCameraId() const              { return cameraId; }

    /** Name of the active source ("No source" when there is none). */
    juce::String getSourceDescription() const;
    ///@}

private:
    void timerCallback() override;
    void handleIncomingFrame (const juce::Image& frame);   // any capture thread
    void processAndPublish();
    bool startSource (std::unique_ptr<FrameSource> newSource, SourceType type);

    std::unique_ptr<FrameSource> source;
    SourceType sourceType = SourceType::none;
    juce::File sourceFile;
    juce::String cameraId;

    juce::CriticalSection rawLock;
    juce::Image rawFrame;          // latest frame handle from the source
    bool rawDirty = false;

    ProcessedFrame processed;      // message thread only

    std::atomic<float> brightness { 0.0f }, contrast { 0.0f }, saturation { 1.0f };
    std::atomic<bool> mirrorH { false }, mirrorV { false };
    std::atomic<int> rateHz { 30 };
    std::atomic<int> workingMax { 0 };
    std::atomic<bool> forceRefresh { false };

    // Timer-side change detection.
    ImageAdjustments lastAdjustments;
    int lastWorkingMax = 0;
    int lastRateHz = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VideoEngine)
};

} // namespace fxme
