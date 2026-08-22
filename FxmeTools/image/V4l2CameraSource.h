/*
  ------------------------------------------------------------------------------
    V4l2CameraSource.h

    Linux webcam FrameSource built directly on the V4L2 kernel API.

    JUCE's juce::CameraDevice is implemented on Windows and macOS only, so on
    Linux this is the camera backend (fxme::VideoEngine picks it for you).
    V4L2 is an ioctl interface, not a library: the only build requirement is
    <linux/videodev2.h>, which every Linux toolchain ships — nothing to
    vendor, nothing to link.

    Captures YUYV 4:2:2 and converts to ARGB itself (integer BT.601), so no
    OpenCV or libv4l dependency is pulled in. Frames arrive on a private
    capture thread.

    Compiles to an empty translation unit on non-Linux platforms.

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#if defined (__linux__)

#include "FrameSource.h"
#include <juce_core/juce_core.h>
#include <vector>

namespace fxme
{

/**
 * @class V4l2CameraSource
 * @brief Webcam capture through V4L2 (Linux), YUYV to ARGB.
 */
class V4l2CameraSource : public FrameSource,
                         private juce::Thread
{
public:
    /** A capture device found by getAvailableDevices(). */
    struct DeviceInfo
    {
        juce::String name;   ///< human-readable card name
        juce::String path;   ///< /dev/videoN — pass this to the constructor
    };

    /** Scans /dev/video0..63 for capture-capable devices. */
    static std::vector<DeviceInfo> getAvailableDevices();

    /** @param devicePath      a path from getAvailableDevices()
        @param requestedWidth  preferred capture size; the driver may adjust it
        @param requestedHeight (query getName() after start() for the card name) */
    V4l2CameraSource (const juce::String& devicePath,
                      int requestedWidth = 640, int requestedHeight = 480);
    ~V4l2CameraSource() override;

    bool start() override;
    void stop() override;

    juce::String getName() const override { return deviceName; }
    bool isLive() const override          { return true; }

    juce::String getDevicePath() const    { return devicePath; }
    int getWidth() const                  { return imageWidth; }
    int getHeight() const                 { return imageHeight; }

private:
    void run() override;
    void emitFrame (const unsigned char* yuyv, int width, int height);

    juce::String devicePath, deviceName;
    int videoFd = -1;
    int imageWidth, imageHeight;

    struct MappedBuffer { void* start = nullptr; size_t length = 0; };
    std::vector<MappedBuffer> buffers;
};

} // namespace fxme

#endif // __linux__
