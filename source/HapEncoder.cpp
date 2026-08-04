#include "HapEncoder.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace
{

/**
    A one-shot RGBA reader for a single video file: opens it, then hands out
    frames in order via next(). Wraps the FFmpeg demux/decode/scale chain so the
    encode loop can stay readable.
*/
class RgbaReader
{
public:
    ~RgbaReader() { close(); }

    bool open (const juce::File& file, juce::String& error)
    {
        const auto path = file.getFullPathName();

        if (avformat_open_input (&fmt, path.toRawUTF8(), nullptr, nullptr) != 0)
        {
            error = "Could not open " + file.getFileName();
            return false;
        }

        if (avformat_find_stream_info (fmt, nullptr) < 0)
        {
            error = "Could not read stream info from " + file.getFileName();
            return false;
        }

        const AVCodec* codec = nullptr;
        streamIndex = av_find_best_stream (fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);

        if (streamIndex < 0 || codec == nullptr)
        {
            error = "No video stream in " + file.getFileName();
            return false;
        }

        auto* stream = fmt->streams[streamIndex];

        dec = avcodec_alloc_context3 (codec);
        if (dec == nullptr
            || avcodec_parameters_to_context (dec, stream->codecpar) < 0
            || avcodec_open2 (dec, codec, nullptr) < 0)
        {
            error = "Could not open the decoder for " + file.getFileName();
            return false;
        }

        // Let the decoder use all cores; these are whole-file batch reads.
        dec->thread_count = 0;

        width  = dec->width;
        height = dec->height;

        if (width <= 0 || height <= 0)
        {
            error = "Invalid dimensions in " + file.getFileName();
            return false;
        }

        const AVRational fr = av_guess_frame_rate (fmt, stream, nullptr);
        frameRate = (fr.num > 0 && fr.den > 0) ? av_q2d (fr) : 30.0;

        if (stream->nb_frames > 0)
            frameCount = (juce::int64) stream->nb_frames;
        else if (fmt->duration > 0)
            frameCount = (juce::int64) ((double) fmt->duration / AV_TIME_BASE * frameRate);

        frame  = av_frame_alloc();
        rgb    = av_frame_alloc();
        packet = av_packet_alloc();

        if (frame == nullptr || rgb == nullptr || packet == nullptr)
        {
            error = "Out of memory setting up " + file.getFileName();
            return false;
        }

        codecName = codec->name;
        return true;
    }

    /** Scales into `w` x `h` RGBA. Must be called before the first next(). */
    bool setOutputSize (int w, int h, juce::String& error)
    {
        outWidth  = w;
        outHeight = h;

        sws = sws_getContext (width, height, dec->pix_fmt,
                              outWidth, outHeight, AV_PIX_FMT_RGBA,
                              SWS_BILINEAR, nullptr, nullptr, nullptr);

        if (sws == nullptr)
        {
            error = "Could not create the colour converter";
            return false;
        }

        if (av_image_alloc (rgb->data, rgb->linesize,
                            outWidth, outHeight, AV_PIX_FMT_RGBA, 1) < 0)
        {
            error = "Could not allocate the RGBA conversion buffer";
            return false;
        }

        rgbAllocated = true;
        return true;
    }

    /**
        Fills `out` (tightly packed RGBA, outWidth * outHeight * 4) with the next
        frame. Returns false at end of stream.
    */
    bool next (std::vector<juce::uint8>& out)
    {
        while (! finished)
        {
            // Drain anything the decoder still holds before reading more.
            int ret = avcodec_receive_frame (dec, frame);

            if (ret == 0)
            {
                convertInto (out);
                return true;
            }

            if (ret != AVERROR (EAGAIN) && ret != AVERROR_EOF)
                return false;

            if (ret == AVERROR_EOF)
            {
                finished = true;
                return false;
            }

            // Decoder wants more input.
            if (draining)
            {
                finished = true;
                return false;
            }

            ret = av_read_frame (fmt, packet);

            if (ret < 0)
            {
                // End of file: flush the decoder so buffered frames come out.
                avcodec_send_packet (dec, nullptr);
                draining = true;
                continue;
            }

            if (packet->stream_index == streamIndex)
                avcodec_send_packet (dec, packet);

            av_packet_unref (packet);
        }

        return false;
    }

    int    getWidth()      const noexcept { return width; }
    int    getHeight()     const noexcept { return height; }
    double getFrameRate()  const noexcept { return frameRate; }
    juce::int64 getFrameCount() const noexcept { return frameCount; }
    juce::String getCodecName() const { return codecName; }

    double getDurationSeconds() const noexcept
    {
        return (fmt != nullptr && fmt->duration > 0)
                 ? (double) fmt->duration / AV_TIME_BASE
                 : 0.0;
    }

    void close()
    {
        if (sws)    { sws_freeContext (sws); sws = nullptr; }
        if (rgbAllocated) { av_freep (&rgb->data[0]); rgbAllocated = false; }
        if (rgb)    { av_frame_free (&rgb); }
        if (frame)  { av_frame_free (&frame); }
        if (packet) { av_packet_free (&packet); }
        if (dec)    { avcodec_free_context (&dec); }
        if (fmt)    { avformat_close_input (&fmt); }
    }

private:
    void convertInto (std::vector<juce::uint8>& out)
    {
        sws_scale (sws, frame->data, frame->linesize, 0, height,
                   rgb->data, rgb->linesize);

        const size_t rowBytes = (size_t) outWidth * 4;
        const size_t stride   = (size_t) rgb->linesize[0];

        out.resize (rowBytes * (size_t) outHeight);

        // swscale's stride may be padded; pack it down for the encoder.
        for (size_t y = 0; y < (size_t) outHeight; ++y)
            std::memcpy (out.data() + y * rowBytes,
                         rgb->data[0] + y * stride,
                         rowBytes);
    }

    AVFormatContext* fmt    = nullptr;
    AVCodecContext*  dec    = nullptr;
    SwsContext*      sws    = nullptr;
    AVFrame*         frame  = nullptr;
    AVFrame*         rgb    = nullptr;
    AVPacket*        packet = nullptr;

    int    streamIndex = -1;
    int    width = 0, height = 0;
    int    outWidth = 0, outHeight = 0;
    double frameRate = 30.0;
    juce::int64 frameCount = 0;
    juce::String codecName;

    bool rgbAllocated = false;
    bool draining = false;
    bool finished = false;
};

/** Rec.709 luma, matching what compositing tools use for a luminance matte. */
inline juce::uint8 luminanceOf (juce::uint8 r, juce::uint8 g, juce::uint8 b) noexcept
{
    return (juce::uint8) ((2126 * (int) r + 7152 * (int) g + 722 * (int) b) / 10000);
}

} // namespace

//==============================================================================

juce::String HapEncoder::ClipInfo::describe() const
{
    if (! valid)
        return "Not a readable video file";

    juce::String s;
    s << width << " x " << height
      << "  |  " << juce::String (frameRate, 2) << " fps";

    if (durationSeconds > 0.0)
        s << "  |  " << juce::String (durationSeconds, 2) << " s";

    if (codecName.isNotEmpty())
        s << "  |  " << codecName;

    return s;
}

HapEncoder::HapEncoder() : juce::Thread ("HapEncode") {}

HapEncoder::~HapEncoder()
{
    cancel();
}

HapEncoder::ClipInfo HapEncoder::probe (const juce::File& file)
{
    ClipInfo info;

    if (! file.existsAsFile())
        return info;

    RgbaReader reader;
    juce::String error;

    if (! reader.open (file, error))
        return info;

    info.valid           = true;
    info.width           = reader.getWidth();
    info.height          = reader.getHeight();
    info.frameRate       = reader.getFrameRate();
    info.durationSeconds = reader.getDurationSeconds();
    info.codecName       = reader.getCodecName();

    return info;
}

bool HapEncoder::start (const Settings& settings, juce::String& error)
{
    if (isThreadRunning())
    {
        error = "An encode is already running.";
        return false;
    }

    if (! settings.colourFile.existsAsFile())
    {
        error = "Pick a colour clip first.";
        return false;
    }

    if (! settings.matteFile.existsAsFile())
    {
        error = "Pick a matte clip first.";
        return false;
    }

    if (settings.outputFile == juce::File())
    {
        error = "Choose where to save the result.";
        return false;
    }

    current   = settings;
    cancelled = false;

    startThread (juce::Thread::Priority::normal);
    return true;
}

void HapEncoder::cancel()
{
    cancelled = true;
    signalThreadShouldExit();
    stopThread (5000);
}

void HapEncoder::postProgress (double progress, const juce::String& status)
{
    if (onProgress == nullptr)
        return;

    juce::MessageManager::callAsync ([cb = onProgress, progress, status]
    {
        cb (progress, status);
    });
}

void HapEncoder::run()
{
    juce::String error;
    const bool ok = encode (error);

    // A cancel isn't a failure — report it as its own outcome.
    const bool wasCancelled = cancelled.load();

    if (! ok && ! wasCancelled)
        current.outputFile.deleteFile();

    if (onFinished != nullptr)
    {
        const auto message = wasCancelled ? juce::String ("Encode cancelled.")
                           : ok           ? "Wrote " + current.outputFile.getFileName()
                                          : error;

        juce::MessageManager::callAsync ([cb = onFinished, ok, wasCancelled, message]
        {
            cb (ok && ! wasCancelled, message);
        });
    }
}

bool HapEncoder::encode (juce::String& error)
{
    postProgress (0.0, "Opening clips...");

    RgbaReader colour, matte;

    if (! colour.open (current.colourFile, error))
        return false;

    if (! matte.open (current.matteFile, error))
        return false;

    // The colour clip defines the output geometry and timing; the matte is
    // rescaled to match so mismatched sources still line up.
    const int    width     = colour.getWidth();
    const int    height    = colour.getHeight();
    const double frameRate = colour.getFrameRate();

    if (! colour.setOutputSize (width, height, error))
        return false;

    if (! matte.setOutputSize (width, height, error))
        return false;

    // Work out how many frames we expect, for progress reporting.
    const juce::int64 colourFrames = colour.getFrameCount();
    const juce::int64 matteFrames  = matte.getFrameCount();
    juce::int64 expectedFrames = 0;

    switch (current.lengthMode)
    {
        case LengthMode::shortest:
            expectedFrames = (colourFrames > 0 && matteFrames > 0)
                               ? juce::jmin (colourFrames, matteFrames) : 0;
            break;
        case LengthMode::colour: expectedFrames = colourFrames; break;
        case LengthMode::matte:  expectedFrames = matteFrames;  break;
    }

    // ---- Set up the Hap encoder -------------------------------------------
    const AVCodec* codec = avcodec_find_encoder_by_name ("hap");

    if (codec == nullptr)
    {
        error = "This build has no Hap encoder.";
        return false;
    }

    AVCodecContext* enc = avcodec_alloc_context3 (codec);

    if (enc == nullptr)
    {
        error = "Could not allocate the Hap encoder.";
        return false;
    }

    // Hap works on 4x4 texture blocks, so both dimensions must be multiples of
    // four. Round up and letterbox rather than refusing the file.
    const int encWidth  = (width  + 3) & ~3;
    const int encHeight = (height + 3) & ~3;

    enc->width     = encWidth;
    enc->height    = encHeight;
    enc->pix_fmt   = AV_PIX_FMT_RGBA;
    enc->time_base = av_d2q (1.0 / frameRate, 90000);
    enc->framerate = av_d2q (frameRate, 90000);

    // hap_alpha = DXT5, which carries the alpha channel we're building.
    av_opt_set (enc->priv_data, "format", "hap_alpha", 0);
    av_opt_set (enc->priv_data, "compressor", "snappy", 0);
    av_opt_set_int (enc->priv_data, "chunks", current.chunkCount, 0);

    // ---- Set up the MOV muxer ---------------------------------------------
    AVFormatContext* out = nullptr;
    const auto outPath = current.outputFile.getFullPathName();

    if (avformat_alloc_output_context2 (&out, nullptr, "mov", outPath.toRawUTF8()) < 0
        || out == nullptr)
    {
        avcodec_free_context (&enc);
        error = "Could not create the output file context.";
        return false;
    }

    AVStream* stream = avformat_new_stream (out, nullptr);

    if (stream == nullptr)
    {
        avcodec_free_context (&enc);
        avformat_free_context (out);
        error = "Could not create the output video stream.";
        return false;
    }

    if (out->oformat->flags & AVFMT_GLOBALHEADER)
        enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2 (enc, codec, nullptr) < 0)
    {
        avcodec_free_context (&enc);
        avformat_free_context (out);
        error = "Could not open the Hap encoder.";
        return false;
    }

    avcodec_parameters_from_context (stream->codecpar, enc);
    stream->time_base = enc->time_base;

    if (avio_open (&out->pb, outPath.toRawUTF8(), AVIO_FLAG_WRITE) < 0)
    {
        avcodec_free_context (&enc);
        avformat_free_context (out);
        error = "Could not open " + current.outputFile.getFileName() + " for writing.";
        return false;
    }

    if (avformat_write_header (out, nullptr) < 0)
    {
        avcodec_free_context (&enc);
        avio_closep (&out->pb);
        avformat_free_context (out);
        error = "Could not write the movie header.";
        return false;
    }

    // ---- Encode loop -------------------------------------------------------
    AVFrame*  encFrame = av_frame_alloc();
    AVPacket* pkt      = av_packet_alloc();
    bool success = true;

    if (encFrame == nullptr || pkt == nullptr)
    {
        error = "Out of memory setting up the encoder.";
        success = false;
    }
    else
    {
        encFrame->format = AV_PIX_FMT_RGBA;
        encFrame->width  = encWidth;
        encFrame->height = encHeight;

        if (av_frame_get_buffer (encFrame, 0) < 0)
        {
            error = "Could not allocate the encode frame buffer.";
            success = false;
        }
    }

    std::vector<juce::uint8> colourPixels, mattePixels;
    std::vector<juce::uint8> lastColour,   lastMatte;
    juce::int64 frameIndex = 0;

    auto writePacket = [&] (AVPacket* p) -> bool
    {
        av_packet_rescale_ts (p, enc->time_base, stream->time_base);
        p->stream_index = stream->index;
        return av_interleaved_write_frame (out, p) >= 0;
    };

    // Pulls one encoded packet at a time and muxes it.
    auto drainEncoder = [&] (bool flush) -> bool
    {
        for (;;)
        {
            const int ret = avcodec_receive_packet (enc, pkt);

            if (ret == AVERROR (EAGAIN) || ret == AVERROR_EOF)
                return true;

            if (ret < 0)
                return false;

            const bool written = writePacket (pkt);
            av_packet_unref (pkt);

            if (! written)
                return false;

            if (! flush)
                return true;
        }
    };

    while (success && ! threadShouldExit())
    {
        const bool haveColour = colour.next (colourPixels);
        const bool haveMatte  = matte.next (mattePixels);

        // Decide whether this frame exists at all, and hold the last frame of
        // whichever clip ran out when the mode says to keep going.
        if (! haveColour && ! haveMatte)
            break;

        if (! haveColour)
        {
            if (current.lengthMode != LengthMode::matte || lastColour.empty())
                break;

            colourPixels = lastColour;
        }

        if (! haveMatte)
        {
            if (current.lengthMode != LengthMode::colour || lastMatte.empty())
                break;

            mattePixels = lastMatte;
        }

        if (haveColour) lastColour = colourPixels;
        if (haveMatte)  lastMatte  = mattePixels;

        if (colourPixels.empty() || mattePixels.empty())
            break;

        if (av_frame_make_writable (encFrame) < 0)
        {
            error = "Could not write into the encode frame.";
            success = false;
            break;
        }

        // ---- Composite: colour RGB + matte-derived alpha -------------------
        const size_t srcRow = (size_t) width * 4;
        const size_t dstRow = (size_t) encFrame->linesize[0];

        for (int y = 0; y < encHeight; ++y)
        {
            auto* dst = encFrame->data[0] + (size_t) y * dstRow;

            // Padding rows when rounding up to a multiple of 4: fully transparent.
            if (y >= height)
            {
                std::memset (dst, 0, (size_t) encWidth * 4);
                continue;
            }

            const auto* c = colourPixels.data() + (size_t) y * srcRow;
            const auto* m = mattePixels.data()  + (size_t) y * srcRow;

            for (int x = 0; x < encWidth; ++x)
            {
                if (x >= width)
                {
                    std::memset (dst + (size_t) x * 4, 0, 4);
                    continue;
                }

                const auto* cp = c + (size_t) x * 4;
                const auto* mp = m + (size_t) x * 4;

                juce::uint8 a = 0;

                switch (current.matteChannel)
                {
                    case MatteChannel::luminance: a = luminanceOf (mp[0], mp[1], mp[2]); break;
                    case MatteChannel::red:       a = mp[0]; break;
                    case MatteChannel::green:     a = mp[1]; break;
                    case MatteChannel::blue:      a = mp[2]; break;
                    case MatteChannel::alpha:     a = mp[3]; break;
                }

                if (current.invertMatte)
                    a = (juce::uint8) (255 - a);

                // Premultiplied against black: scale RGB by alpha so masked-out
                // areas go to black rather than keeping their original colour.
                // +127 rounds to nearest instead of truncating.
                auto* d = dst + (size_t) x * 4;
                d[0] = (juce::uint8) ((cp[0] * a + 127) / 255);
                d[1] = (juce::uint8) ((cp[1] * a + 127) / 255);
                d[2] = (juce::uint8) ((cp[2] * a + 127) / 255);
                d[3] = a;
            }
        }

        encFrame->pts = frameIndex;

        if (avcodec_send_frame (enc, encFrame) < 0)
        {
            error = "The Hap encoder rejected a frame.";
            success = false;
            break;
        }

        if (! drainEncoder (false))
        {
            error = "Could not write to the output file.";
            success = false;
            break;
        }

        ++frameIndex;

        if ((frameIndex % 4) == 0 || frameIndex == 1)
        {
            const double progress = expectedFrames > 0
                                      ? juce::jlimit (0.0, 0.999,
                                            (double) frameIndex / (double) expectedFrames)
                                      : -1.0;

            juce::String status;
            status << "Encoding frame " << frameIndex;

            if (expectedFrames > 0)
                status << " of " << expectedFrames;

            postProgress (progress, status);
        }
    }

    // ---- Flush and finalise ------------------------------------------------
    if (success && ! threadShouldExit())
    {
        postProgress (0.999, "Finalising...");

        avcodec_send_frame (enc, nullptr);

        if (! drainEncoder (true))
        {
            error = "Could not flush the encoder.";
            success = false;
        }
        else if (av_write_trailer (out) < 0)
        {
            error = "Could not finalise the movie file.";
            success = false;
        }
    }
    else if (success)
    {
        // Cancelled: the file is incomplete, so don't claim success.
        success = false;
    }

    if (success && frameIndex == 0)
    {
        error = "No frames could be read from the source clips.";
        success = false;
    }

    if (pkt)      av_packet_free (&pkt);
    if (encFrame) av_frame_free (&encFrame);

    avcodec_free_context (&enc);

    if (out->pb != nullptr)
        avio_closep (&out->pb);

    avformat_free_context (out);

    if (success)
        postProgress (1.0, "Done — " + juce::String (frameIndex) + " frames.");

    return success;
}
