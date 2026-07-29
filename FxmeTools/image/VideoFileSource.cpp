/*
  ------------------------------------------------------------------------------
    VideoFileSource.cpp — see header.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include "VideoFileSource.h"

#if FXME_HAS_FFMPEG

extern "C"
{
 #include <libavformat/avformat.h>
 #include <libavcodec/avcodec.h>
 #include <libavutil/imgutils.h>
 #include <libswscale/swscale.h>
}

namespace fxme
{

struct VideoFileSource::Impl
{
    AVFormatContext* fmt = nullptr;
    AVCodecContext* codec = nullptr;
    SwsContext* sws = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    int streamIndex = -1;
    AVRational timeBase {};
    double fallbackFrameSeconds = 1.0 / 25.0;   // used when frames carry no pts
    double duration = 0.0;
    int outW = 0, outH = 0;

    ~Impl() { close(); }

    bool open (const juce::File& file, int maxDimension)
    {
        close();

        if (avformat_open_input (&fmt, file.getFullPathName().toRawUTF8(), nullptr, nullptr) < 0)
            return false;

        if (avformat_find_stream_info (fmt, nullptr) < 0)
            return false;

        streamIndex = av_find_best_stream (fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (streamIndex < 0)
            return false;

        auto* stream = fmt->streams[streamIndex];
        timeBase = stream->time_base;

        if (stream->duration > 0)
            duration = (double) stream->duration * av_q2d (timeBase);
        else if (fmt->duration > 0)
            duration = (double) fmt->duration / (double) AV_TIME_BASE;

        const AVRational fr = av_guess_frame_rate (fmt, stream, nullptr);
        if (fr.num > 0 && fr.den > 0)
            fallbackFrameSeconds = (double) fr.den / (double) fr.num;

        const AVCodec* decoder = avcodec_find_decoder (stream->codecpar->codec_id);
        if (decoder == nullptr)
            return false;

        codec = avcodec_alloc_context3 (decoder);
        if (codec == nullptr
            || avcodec_parameters_to_context (codec, stream->codecpar) < 0
            || avcodec_open2 (codec, decoder, nullptr) < 0)
            return false;

        if (codec->width <= 0 || codec->height <= 0)
            return false;

        const float scale = maxDimension > 0
                              ? juce::jmin (1.0f, (float) maxDimension
                                                    / (float) juce::jmax (codec->width, codec->height))
                              : 1.0f;
        outW = juce::jmax (2, (juce::roundToInt ((float) codec->width  * scale) / 2) * 2);
        outH = juce::jmax (2, (juce::roundToInt ((float) codec->height * scale) / 2) * 2);

        // juce::Image ARGB is BGRA in memory on little-endian — matches AV_PIX_FMT_BGRA.
        sws = sws_getContext (codec->width, codec->height, codec->pix_fmt,
                              outW, outH, AV_PIX_FMT_BGRA,
                              SWS_BILINEAR, nullptr, nullptr, nullptr);

        frame = av_frame_alloc();
        packet = av_packet_alloc();

        return sws != nullptr && frame != nullptr && packet != nullptr;
    }

    void close()
    {
        if (packet != nullptr) av_packet_free (&packet);
        if (frame != nullptr)  av_frame_free (&frame);
        if (sws != nullptr)    { sws_freeContext (sws); sws = nullptr; }
        if (codec != nullptr)  avcodec_free_context (&codec);
        if (fmt != nullptr)    avformat_close_input (&fmt);
        streamIndex = -1;
        duration = 0.0;
    }

    void seekToStart()
    {
        av_seek_frame (fmt, streamIndex, 0, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers (codec);
    }

    juce::Image convert (const AVFrame& src) const
    {
        juce::Image image (juce::Image::ARGB, outW, outH, false);
        juce::Image::BitmapData dest (image, juce::Image::BitmapData::writeOnly);

        juce::uint8* dstData[4] = { dest.data, nullptr, nullptr, nullptr };
        const int dstStride[4] = { dest.lineStride, 0, 0, 0 };

        sws_scale (sws, src.data, src.linesize, 0, src.height, dstData, dstStride);
        return image;
    }
};

VideoFileSource::VideoFileSource (const juce::File& fileToLoad)
    : juce::Thread ("VideoDecode"),
      impl (std::make_unique<Impl>()),
      file (fileToLoad)
{
}

VideoFileSource::~VideoFileSource()
{
    stop();
}

bool VideoFileSource::start()
{
    if (isThreadRunning())
        return true;

    if (! file.existsAsFile() || ! impl->open (file, maxOutputDimension))
        return false;

    durationSeconds.store (impl->duration);
    positionSeconds.store (0.0);

    startThread();
    return true;
}

void VideoFileSource::stop()
{
    stopThread (3000);
    impl->close();
}

void VideoFileSource::run()
{
    // Wall-clock origin of media time 0. Pausing and looping shift it.
    double originMs = juce::Time::getMillisecondCounterHiRes();
    double lastPtsSeconds = 0.0;

    // Waits until `mediaSeconds` is due, servicing pause and shutdown.
    // Returns false if the thread should exit.
    auto waitUntilDue = [&] (double mediaSeconds) -> bool
    {
        for (;;)
        {
            if (threadShouldExit())
                return false;

            if (paused.load())
            {
                const double pauseStart = juce::Time::getMillisecondCounterHiRes();
                while (paused.load() && ! threadShouldExit())
                    wait (50);
                originMs += juce::Time::getMillisecondCounterHiRes() - pauseStart;
                continue;
            }

            const double dueMs = originMs + mediaSeconds * 1000.0;
            const double nowMs = juce::Time::getMillisecondCounterHiRes();
            if (nowMs >= dueMs)
                return true;

            wait (juce::jlimit (1, 50, (int) (dueMs - nowMs)));
        }
    };

    while (! threadShouldExit())
    {
        const int readResult = av_read_frame (impl->fmt, impl->packet);

        if (readResult < 0)   // EOF (or error)
        {
            if (! looping.load())
                return;

            impl->seekToStart();
            originMs = juce::Time::getMillisecondCounterHiRes();
            lastPtsSeconds = 0.0;
            continue;
        }

        if (impl->packet->stream_index != impl->streamIndex)
        {
            av_packet_unref (impl->packet);
            continue;
        }

        const int sendResult = avcodec_send_packet (impl->codec, impl->packet);
        av_packet_unref (impl->packet);
        if (sendResult < 0)
            continue;

        while (avcodec_receive_frame (impl->codec, impl->frame) >= 0 && ! threadShouldExit())
        {
            const juce::int64 pts = impl->frame->best_effort_timestamp;
            const double ptsSeconds = (pts != AV_NOPTS_VALUE)
                                        ? (double) pts * av_q2d (impl->timeBase)
                                        : lastPtsSeconds + impl->fallbackFrameSeconds;
            lastPtsSeconds = ptsSeconds;

            if (! waitUntilDue (ptsSeconds))
            {
                av_frame_unref (impl->frame);
                return;
            }

            positionSeconds.store (ptsSeconds);

            if (onFrame != nullptr)
                onFrame (impl->convert (*impl->frame));

            av_frame_unref (impl->frame);
        }
    }
}

} // namespace fxme

#else // ─── built without FFmpeg: inert stub ─────────────────────────────────

namespace fxme
{

struct VideoFileSource::Impl {};

VideoFileSource::VideoFileSource (const juce::File& fileToLoad)
    : juce::Thread ("VideoDecode"), file (fileToLoad)
{
}

VideoFileSource::~VideoFileSource() = default;

bool VideoFileSource::start() { return false; }
void VideoFileSource::stop() {}
void VideoFileSource::run() {}

} // namespace fxme

#endif // FXME_HAS_FFMPEG
