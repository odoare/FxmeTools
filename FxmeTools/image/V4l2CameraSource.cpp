/*
  ------------------------------------------------------------------------------
    V4l2CameraSource.cpp — see header.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#if defined (__linux__)

#include "V4l2CameraSource.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <cerrno>
#include <string>

namespace fxme
{

std::vector<V4l2CameraSource::DeviceInfo> V4l2CameraSource::getAvailableDevices()
{
    std::vector<DeviceInfo> devices;

    for (int i = 0; i < 64; ++i)
    {
        const std::string path = "/dev/video" + std::to_string (i);
        const int fd = ::open (path.c_str(), O_RDWR);
        if (fd < 0)
            continue;

        v4l2_capability cap = {};
        if (ioctl (fd, VIDIOC_QUERYCAP, &cap) >= 0
            && (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) != 0)
        {
            devices.push_back ({ juce::String ((const char*) cap.card),
                                 juce::String (path) });
        }
        ::close (fd);
    }
    return devices;
}

V4l2CameraSource::V4l2CameraSource (const juce::String& path, int w, int h)
    : juce::Thread ("V4l2Camera"),
      devicePath (path), deviceName (path),
      imageWidth (w), imageHeight (h)
{
}

V4l2CameraSource::~V4l2CameraSource()
{
    stop();
}

bool V4l2CameraSource::start()
{
    if (videoFd >= 0)
        return true;

    videoFd = ::open (devicePath.toRawUTF8(), O_RDWR);
    if (videoFd < 0)
        return false;

    v4l2_capability cap = {};
    if (ioctl (videoFd, VIDIOC_QUERYCAP, &cap) >= 0)
        deviceName = juce::String ((const char*) cap.card);

    v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = (unsigned) imageWidth;
    fmt.fmt.pix.height = (unsigned) imageHeight;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl (videoFd, VIDIOC_S_FMT, &fmt) < 0)
    {
        ::close (videoFd);
        videoFd = -1;
        return false;
    }

    // The driver may have adjusted the size.
    imageWidth  = (int) fmt.fmt.pix.width;
    imageHeight = (int) fmt.fmt.pix.height;

    v4l2_requestbuffers req = {};
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl (videoFd, VIDIOC_REQBUFS, &req) < 0)
    {
        ::close (videoFd);
        videoFd = -1;
        return false;
    }

    buffers.resize (req.count);

    for (size_t i = 0; i < buffers.size(); ++i)
    {
        v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = (unsigned) i;

        if (ioctl (videoFd, VIDIOC_QUERYBUF, &buf) < 0)
            continue;

        buffers[i].length = buf.length;
        buffers[i].start = mmap (nullptr, buf.length, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, videoFd, buf.m.offset);
    }

    for (size_t i = 0; i < buffers.size(); ++i)
    {
        v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = (unsigned) i;
        ioctl (videoFd, VIDIOC_QBUF, &buf);
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl (videoFd, VIDIOC_STREAMON, &type);

    startThread();
    return true;
}

void V4l2CameraSource::stop()
{
    stopThread (2000);

    if (videoFd >= 0)
    {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl (videoFd, VIDIOC_STREAMOFF, &type);

        for (auto& b : buffers)
            if (b.start != nullptr)
                munmap (b.start, b.length);
        buffers.clear();

        ::close (videoFd);
        videoFd = -1;
    }
}

void V4l2CameraSource::run()
{
    while (! threadShouldExit())
    {
        if (videoFd < 0)
            break;

        fd_set fds;
        FD_ZERO (&fds);
        FD_SET (videoFd, &fds);

        timeval tv = {};
        tv.tv_sec = 1;

        if (select (videoFd + 1, &fds, nullptr, nullptr, &tv) <= 0)
            continue;

        v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl (videoFd, VIDIOC_DQBUF, &buf) >= 0)
        {
            if (buf.index < buffers.size() && onFrame != nullptr)
                emitFrame ((const unsigned char*) buffers[buf.index].start,
                           imageWidth, imageHeight);

            ioctl (videoFd, VIDIOC_QBUF, &buf);
        }
    }
}

// YUYV 4:2:2 -> ARGB (BT.601, integer maths). Runs on the capture thread.
void V4l2CameraSource::emitFrame (const unsigned char* yuyv, int width, int height)
{
    juce::Image image (juce::Image::ARGB, width, height, false);
    juce::Image::BitmapData dest (image, juce::Image::BitmapData::writeOnly);

    auto clamp255 = [] (int v) -> juce::uint8
    {
        return (juce::uint8) (v < 0 ? 0 : (v > 255 ? 255 : v));
    };

    for (int y = 0; y < height; ++y)
    {
        const unsigned char* src = yuyv + (size_t) y * (size_t) width * 2;
        juce::uint8* dst = dest.getLinePointer (y);

        for (int x = 0; x < width; x += 2)
        {
            const int y0 = src[0], u = src[1], y1 = src[2], v = src[3];
            src += 4;

            const int d = u - 128, e = v - 128;
            const int rOff =  409 * e + 128;
            const int gOff = -100 * d - 208 * e + 128;
            const int bOff =  516 * d + 128;

            for (int k = 0; k < 2; ++k)
            {
                const int c = 298 * ((k == 0 ? y0 : y1) - 16);
                dst[0] = clamp255 ((c + bOff) >> 8);   // B
                dst[1] = clamp255 ((c + gOff) >> 8);   // G
                dst[2] = clamp255 ((c + rOff) >> 8);   // R
                dst[3] = 255;                          // A
                dst += 4;
            }
        }
    }

    onFrame (image);
}

} // namespace fxme

#endif // __linux__
