/*
  ------------------------------------------------------------------------------
    VideoEngine.cpp — see header.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include "VideoEngine.h"
#include "StillImageSource.h"
#include "VideoFileSource.h"

#if defined (__linux__)
 #include "V4l2CameraSource.h"
#endif
#if FXME_HAS_JUCE_CAMERA
 #include "JuceCameraSource.h"
#endif

namespace fxme
{

VideoEngine::VideoEngine()
{
    lastRateHz = rateHz.load();
    startTimerHz (lastRateHz);
}

VideoEngine::~VideoEngine()
{
    closeSource();
}

//==============================================================================
bool VideoEngine::startSource (std::unique_ptr<FrameSource> newSource, SourceType type)
{
    closeSource();

    if (newSource == nullptr)
        return false;

    newSource->onFrame = [this] (const juce::Image& frame) { handleIncomingFrame (frame); };

    if (! newSource->start())
        return false;

    source = std::move (newSource);
    sourceType = type;
    return true;
}

bool VideoEngine::loadImageFile (const juce::File& file)
{
    if (startSource (std::make_unique<StillImageSource> (file), SourceType::stillImage))
    {
        sourceFile = file;
        return true;
    }
    return false;
}

bool VideoEngine::loadImage (const juce::Image& image, const juce::String& name)
{
    return startSource (std::make_unique<StillImageSource> (image, name), SourceType::stillImage);
}

bool VideoEngine::loadVideoFile (const juce::File& file)
{
    auto video = std::make_unique<VideoFileSource> (file);
    if (startSource (std::move (video), SourceType::videoFile))
    {
        sourceFile = file;
        return true;
    }
    return false;
}

bool VideoEngine::openCamera (const juce::String& deviceId)
{
   #if defined (__linux__)
    auto cam = std::make_unique<V4l2CameraSource> (deviceId);
   #elif FXME_HAS_JUCE_CAMERA
    auto cam = std::make_unique<JuceCameraSource> (deviceId.getIntValue());
   #else
    juce::ignoreUnused (deviceId);
    return false;
   #endif

   #if defined (__linux__) || FXME_HAS_JUCE_CAMERA
    if (startSource (std::move (cam), SourceType::camera))
    {
        cameraId = deviceId;
        return true;
    }
    return false;
   #endif
}

bool VideoEngine::setSource (std::unique_ptr<FrameSource> newSource, SourceType type)
{
    return startSource (std::move (newSource), type);
}

void VideoEngine::closeSource()
{
    if (source != nullptr)
    {
        source->stop();
        source.reset();
    }
    sourceType = SourceType::none;
    sourceFile = juce::File();
    cameraId.clear();
}

//==============================================================================
juce::StringArray VideoEngine::getCameraDeviceNames()
{
   #if defined (__linux__)
    juce::StringArray names;
    for (const auto& d : V4l2CameraSource::getAvailableDevices())
        names.add (d.name + " (" + d.path + ")");
    return names;
   #elif FXME_HAS_JUCE_CAMERA
    return JuceCameraSource::getAvailableDevices();
   #else
    return {};
   #endif
}

juce::StringArray VideoEngine::getCameraDeviceIds()
{
   #if defined (__linux__)
    juce::StringArray ids;
    for (const auto& d : V4l2CameraSource::getAvailableDevices())
        ids.add (d.path);
    return ids;
   #elif FXME_HAS_JUCE_CAMERA
    juce::StringArray ids;
    for (int i = 0; i < JuceCameraSource::getAvailableDevices().size(); ++i)
        ids.add (juce::String (i));
    return ids;
   #else
    return {};
   #endif
}

bool VideoEngine::isCameraSupported()
{
   #if defined (__linux__) || FXME_HAS_JUCE_CAMERA
    return true;
   #else
    return false;
   #endif
}

bool VideoEngine::isVideoFileSupported()
{
    return FXME_HAS_FFMPEG != 0;
}

//==============================================================================
void VideoEngine::setVideoPaused (bool shouldPause)
{
    if (auto* video = dynamic_cast<VideoFileSource*> (source.get()))
        video->setPaused (shouldPause);
}

bool VideoEngine::isVideoPaused() const
{
    if (auto* video = dynamic_cast<VideoFileSource*> (source.get()))
        return video->isPaused();
    return false;
}

void VideoEngine::setVideoLooping (bool shouldLoop)
{
    if (auto* video = dynamic_cast<VideoFileSource*> (source.get()))
        video->setLooping (shouldLoop);
}

double VideoEngine::getVideoPositionSeconds() const
{
    if (auto* video = dynamic_cast<VideoFileSource*> (source.get()))
        return video->getPositionSeconds();
    return 0.0;
}

double VideoEngine::getVideoDurationSeconds() const
{
    if (auto* video = dynamic_cast<VideoFileSource*> (source.get()))
        return video->getDurationSeconds();
    return 0.0;
}

juce::String VideoEngine::getSourceDescription() const
{
    return source != nullptr ? source->getName() : juce::String ("No source");
}

//==============================================================================
void VideoEngine::setAdjustments (const ImageAdjustments& a)
{
    brightness.store (a.brightness);
    contrast.store (a.contrast);
    saturation.store (a.saturation);
    mirrorH.store (a.mirrorH);
    mirrorV.store (a.mirrorV);
}

ImageAdjustments VideoEngine::getAdjustments() const
{
    ImageAdjustments a;
    a.brightness = brightness.load();
    a.contrast = contrast.load();
    a.saturation = saturation.load();
    a.mirrorH = mirrorH.load();
    a.mirrorV = mirrorV.load();
    return a;
}

//==============================================================================
void VideoEngine::handleIncomingFrame (const juce::Image& frame)
{
    // Capture threads hand over a freshly created, immutable image, so
    // swapping the ref-counted handle under a short lock is all we need.
    const juce::ScopedLock sl (rawLock);
    rawFrame = frame;
    rawDirty = true;
}

void VideoEngine::timerCallback()
{
    const int rate = rateHz.load();
    if (rate != lastRateHz)
    {
        lastRateHz = rate;
        startTimerHz (rate);
    }

    const auto adjustments = getAdjustments();
    const int working = workingMax.load();

    bool hasNewFrame;
    {
        const juce::ScopedLock sl (rawLock);
        hasNewFrame = rawDirty;
    }

    if (hasNewFrame || adjustments != lastAdjustments || working != lastWorkingMax
        || forceRefresh.exchange (false))
    {
        lastAdjustments = adjustments;
        lastWorkingMax = working;
        processAndPublish();
    }
}

void VideoEngine::processAndPublish()
{
    juce::Image raw;
    {
        const juce::ScopedLock sl (rawLock);
        raw = rawFrame;
        rawDirty = false;
    }

    if (! raw.isValid())
        return;

    processed.image = lastAdjustments.applyTo (raw);

    // Working copy for analysis / DSP: scaled down when a cap is set.
    const int cap = lastWorkingMax;
    const int w = processed.image.getWidth(), h = processed.image.getHeight();

    if (cap > 0 && juce::jmax (w, h) > cap)
    {
        const float scale = (float) cap / (float) juce::jmax (w, h);
        processed.working = processed.image.rescaled (juce::jmax (2, juce::roundToInt ((float) w * scale)),
                                                      juce::jmax (2, juce::roundToInt ((float) h * scale)),
                                                      juce::Graphics::mediumResamplingQuality);
    }
    else
    {
        processed.working = processed.image;
    }

    if (onFrame != nullptr)
        onFrame (processed);

    sendChangeMessage();
}

} // namespace fxme
