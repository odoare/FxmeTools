/*
  ------------------------------------------------------------------------------
    JuceCameraSource.h

    Webcam FrameSource for Windows and macOS, wrapping juce::CameraDevice
    (juce_video module: DirectShow on Windows, AVFoundation on macOS). JUCE
    does not implement CameraDevice on Linux, where fxme::V4l2CameraSource
    takes over — fxme::VideoEngine picks the right backend automatically.

    This header is only compiled when the consumer defines
    FXME_HAS_JUCE_CAMERA=1 and links juce_video (fxmetools_attach_video()
    does both). Otherwise it is an empty translation unit, so FxmeTools never
    forces a juce_video dependency on plugins that do not want one.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#ifndef FXME_HAS_JUCE_CAMERA
 #define FXME_HAS_JUCE_CAMERA 0
#endif

#if FXME_HAS_JUCE_CAMERA

#include "FrameSource.h"
#include <juce_video/juce_video.h>

namespace fxme
{

/**
 * @class JuceCameraSource
 * @brief Webcam capture through juce::CameraDevice (Windows / macOS).
 */
class JuceCameraSource : public FrameSource,
                         private juce::CameraDevice::Listener
{
public:
    /** Device names, in the index order expected by the constructor. */
    static juce::StringArray getAvailableDevices() { return juce::CameraDevice::getAvailableDevices(); }

    explicit JuceCameraSource (int deviceIndexToOpen) : deviceIndex (deviceIndexToOpen) {}

    ~JuceCameraSource() override { stop(); }

    bool start() override
    {
        if (device != nullptr)
            return true;

        device.reset (juce::CameraDevice::openDevice (deviceIndex));
        if (device == nullptr)
            return false;

        device->addListener (this);
        return true;
    }

    void stop() override
    {
        if (device != nullptr)
        {
            device->removeListener (this);
            device.reset();
        }
    }

    juce::String getName() const override { return device != nullptr ? device->getName()
                                                                    : "Camera " + juce::String (deviceIndex); }
    bool isLive() const override          { return true; }

    int getDeviceIndex() const            { return deviceIndex; }

private:
    void imageReceived (const juce::Image& image) override
    {
        if (onFrame != nullptr)
            onFrame (image);
    }

    int deviceIndex = 0;
    std::unique_ptr<juce::CameraDevice> device;
};

} // namespace fxme

#endif // FXME_HAS_JUCE_CAMERA
