#include "HapSplitter.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace
{

/**
    Writes one H.264 .mp4. Both output files use an instance of this, so the
    colour and alpha writers stay identical apart from the pixels fed in.
*/
class Mp4Writer
{
public:
    ~Mp4Writer() { close(); }

    bool open (const juce::File& file, int w, int h, double fps,
               int crf, juce::String& error)
    {
        const auto path = file.getFullPathName();

        const AVCodec* codec = avcodec_find_encoder_by_name ("libx264");

        if (codec == nullptr)
        {
            error = "This build has no H.264 encoder.";
            return false;
        }

        if (avformat_alloc_output_context2 (&fmt, nullptr, "mp4", path.toRawUTF8()) < 0
            || fmt == nullptr)
        {
            error = "Could not create " + file.getFileName();
            return false;
        }

        enc = avcodec_alloc_context3 (codec);

        if (enc == nullptr)
        {
            error = "Could not allocate the H.264 encoder.";
            return false;
        }

        // x264 needs even dimensions for yuv420p chroma subsampling.
        enc->width     = w & ~1;
        enc->height    = h & ~1;
        enc->pix_fmt   = AV_PIX_FMT_YUV420P;
        enc->time_base = av_d2q (1.0 / fps, 90000);
        enc->framerate = av_d2q (fps, 90000);

        av_opt_set_int (enc->priv_data, "crf", crf, 0);
        av_opt_set (enc->priv_data, "preset", "medium", 0);

        stream = avformat_new_stream (fmt, nullptr);

        if (stream == nullptr)
        {
            error = "Could not create the video stream in " + file.getFileName();
            return false;
        }

        if (fmt->oformat->flags & AVFMT_GLOBALHEADER)
            enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        if (avcodec_open2 (enc, codec, nullptr) < 0)
        {
            error = "Could not open the H.264 encoder.";
            return false;
        }

        avcodec_parameters_from_context (stream->codecpar, enc);
        stream->time_base = enc->time_base;

        if (avio_open (&fmt->pb, path.toRawUTF8(), AVIO_FLAG_WRITE) < 0)
        {
            error = "Could not open " + file.getFileName() + " for writing.";
            return false;
        }

        if (avformat_write_header (fmt, nullptr) < 0)
        {
            error = "Could not write the header of " + file.getFileName();
            return false;
        }

        frame = av_frame_alloc();
        pkt   = av_packet_alloc();

        if (frame == nullptr || pkt == nullptr)
        {
            error = "Out of memory setting up " + file.getFileName();
            return false;
        }

        frame->format = AV_PIX_FMT_YUV420P;
        frame->width  = enc->width;
        frame->height = enc->height;

        if (av_frame_get_buffer (frame, 0) < 0)
        {
            error = "Could not allocate the encode buffer.";
            return false;
        }

        sws = sws_getContext (enc->width, enc->height, AV_PIX_FMT_RGBA,
                              enc->width, enc->height, AV_PIX_FMT_YUV420P,
                              SWS_BILINEAR, nullptr, nullptr, nullptr);

        if (sws == nullptr)
        {
            error = "Could not create the colour converter.";
            return false;
        }

        return true;
    }

    /** Feeds one tightly-packed RGBA frame. */
    bool writeFrame (const juce::uint8* rgba, int srcStride, juce::int64 pts)
    {
        if (av_frame_make_writable (frame) < 0)
            return false;

        const juce::uint8* src[4] = { rgba, nullptr, nullptr, nullptr };
        const int stride[4] = { srcStride, 0, 0, 0 };

        sws_scale (sws, src, stride, 0, frame->height, frame->data, frame->linesize);

        frame->pts = pts;

        if (avcodec_send_frame (enc, frame) < 0)
            return false;

        return drain (false);
    }

    bool finish()
    {
        if (fmt == nullptr)
            return false;

        avcodec_send_frame (enc, nullptr);

        if (! drain (true))
            return false;

        return av_write_trailer (fmt) >= 0;
    }

    int getWidth()  const noexcept { return enc != nullptr ? enc->width  : 0; }
    int getHeight() const noexcept { return enc != nullptr ? enc->height : 0; }

    void close()
    {
        if (sws)   { sws_freeContext (sws); sws = nullptr; }
        if (pkt)   { av_packet_free (&pkt); }
        if (frame) { av_frame_free (&frame); }
        if (enc)   { avcodec_free_context (&enc); }

        if (fmt != nullptr)
        {
            if (fmt->pb != nullptr)
                avio_closep (&fmt->pb);

            avformat_free_context (fmt);
            fmt = nullptr;
        }
    }

private:
    bool drain (bool flush)
    {
        for (;;)
        {
            const int ret = avcodec_receive_packet (enc, pkt);

            if (ret == AVERROR (EAGAIN) || ret == AVERROR_EOF)
                return true;

            if (ret < 0)
                return false;

            av_packet_rescale_ts (pkt, enc->time_base, stream->time_base);
            pkt->stream_index = stream->index;

            const bool ok = av_interleaved_write_frame (fmt, pkt) >= 0;
            av_packet_unref (pkt);

            if (! ok)
                return false;

            if (! flush)
                return true;
        }
    }

    AVFormatContext* fmt    = nullptr;
    AVCodecContext*  enc    = nullptr;
    AVStream*        stream = nullptr;
    AVFrame*         frame  = nullptr;
    AVPacket*        pkt    = nullptr;
    SwsContext*      sws    = nullptr;
};

/** True if this pixel format carries an alpha channel. */
bool pixelFormatHasAlpha (AVPixelFormat fmt)
{
    const auto* desc = av_pix_fmt_desc_get (fmt);
    return desc != nullptr && (desc->flags & AV_PIX_FMT_FLAG_ALPHA) != 0;
}

} // namespace

//==============================================================================

HapSplitter::HapSplitter() : juce::Thread ("HapSplit") {}

HapSplitter::~HapSplitter()
{
    cancel();
}

bool HapSplitter::sourceHasAlpha (const juce::File& file)
{
    if (! file.existsAsFile())
        return false;

    AVFormatContext* fmt = nullptr;

    if (avformat_open_input (&fmt, file.getFullPathName().toRawUTF8(),
                             nullptr, nullptr) != 0)
        return false;

    bool result = false;

    if (avformat_find_stream_info (fmt, nullptr) >= 0)
    {
        const AVCodec* codec = nullptr;
        const int idx = av_find_best_stream (fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);

        if (idx >= 0 && codec != nullptr)
        {
            // The stream's declared format is the reliable signal here; Hap
            // Alpha reports rgba, Hap (no alpha) reports rgb.
            auto* par = fmt->streams[idx]->codecpar;
            result = pixelFormatHasAlpha ((AVPixelFormat) par->format);

            // Some codecs leave codecpar->format unset (-1) until a frame is
            // decoded; fall back to the codec's own list of pixel formats.
            if (par->format == AV_PIX_FMT_NONE)
            {
                // AVCodec::pix_fmts was removed in FFmpeg 8 (the vcpkg build);
                // the replacement exists from FFmpeg 7.1.
               #if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT (61, 13, 100)
                const void* configs = nullptr;
                int numConfigs = 0;

                if (avcodec_get_supported_config (nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                                  &configs, &numConfigs) >= 0
                    && configs != nullptr)
                    for (int i = 0; i < numConfigs; ++i)
                        if (pixelFormatHasAlpha (static_cast<const AVPixelFormat*> (configs)[i]))
                            result = true;
               #else
                if (codec->pix_fmts != nullptr)
                    for (const AVPixelFormat* p = codec->pix_fmts; *p != AV_PIX_FMT_NONE; ++p)
                        if (pixelFormatHasAlpha (*p))
                            result = true;
               #endif
            }
        }
    }

    avformat_close_input (&fmt);
    return result;
}

bool HapSplitter::start (const Settings& settings, juce::String& error)
{
    if (isThreadRunning())
    {
        error = "A split is already running.";
        return false;
    }

    if (! settings.sourceFile.existsAsFile())
    {
        error = "Pick a source clip first.";
        return false;
    }

    current   = settings;
    cancelled = false;

    startThread (juce::Thread::Priority::normal);
    return true;
}

void HapSplitter::cancel()
{
    cancelled = true;
    signalThreadShouldExit();
    stopThread (5000);
}

void HapSplitter::run()
{
    juce::String error;
    const bool ok = split (error);
    const bool wasCancelled = cancelled.load();

    if (onFinished != nullptr)
    {
        const auto message = wasCancelled ? juce::String ("Split cancelled.")
                           : ok           ? juce::String ("Done.")
                                          : error;

        juce::MessageManager::callAsync ([cb = onFinished, ok, wasCancelled, message]
        {
            cb (ok && ! wasCancelled, message);
        });
    }
}

bool HapSplitter::split (juce::String& error)
{
    const auto stem = current.sourceFile.getFileNameWithoutExtension();
    const auto dir  = current.outputDirectory != juce::File()
                        ? current.outputDirectory
                        : current.sourceFile.getParentDirectory();

    const auto rgbFile   = dir.getChildFile (stem + "_rgb.mp4");
    const auto alphaFile = dir.getChildFile (stem + "_a.mp4");

    // ---- Open the source ---------------------------------------------------
    AVFormatContext* fmt = nullptr;

    if (avformat_open_input (&fmt, current.sourceFile.getFullPathName().toRawUTF8(),
                             nullptr, nullptr) != 0)
    {
        error = "Could not open " + current.sourceFile.getFileName();
        return false;
    }

    struct Closer
    {
        AVFormatContext** f;
        ~Closer() { if (*f != nullptr) avformat_close_input (f); }
    } closer { &fmt };

    if (avformat_find_stream_info (fmt, nullptr) < 0)
    {
        error = "Could not read stream info.";
        return false;
    }

    const AVCodec* codec = nullptr;
    const int streamIndex = av_find_best_stream (fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);

    if (streamIndex < 0 || codec == nullptr)
    {
        error = "No video stream in " + current.sourceFile.getFileName();
        return false;
    }

    AVCodecContext* dec = avcodec_alloc_context3 (codec);

    if (dec == nullptr
        || avcodec_parameters_to_context (dec, fmt->streams[streamIndex]->codecpar) < 0
        || avcodec_open2 (dec, codec, nullptr) < 0)
    {
        if (dec != nullptr) avcodec_free_context (&dec);
        error = "Could not open the decoder.";
        return false;
    }

    dec->thread_count = 0;

    const int w = dec->width;
    const int h = dec->height;

    if (w <= 0 || h <= 0)
    {
        avcodec_free_context (&dec);
        error = "Invalid video dimensions.";
        return false;
    }

    const AVRational fr = av_guess_frame_rate (fmt, fmt->streams[streamIndex], nullptr);
    const double fps = (fr.num > 0 && fr.den > 0) ? av_q2d (fr) : 30.0;

    juce::int64 expectedFrames = fmt->streams[streamIndex]->nb_frames;

    if (expectedFrames <= 0 && fmt->duration > 0)
        expectedFrames = (juce::int64) ((double) fmt->duration / AV_TIME_BASE * fps);

    // ---- Set up both writers ----------------------------------------------
    // Quality slider -> x264 CRF. 51 is worst, 0 is lossless; 18..28 is the
    // useful band, so map the slider across it inverted (1.0 -> 18).
    const int crf = (int) std::round (juce::jmap (juce::jlimit (0.0, 1.0, current.quality),
                                                  28.0, 18.0));

    Mp4Writer rgbWriter, alphaWriter;

    if (! rgbWriter.open (rgbFile, w, h, fps, crf, error)
        || ! alphaWriter.open (alphaFile, w, h, fps, crf, error))
    {
        avcodec_free_context (&dec);
        return false;
    }

    const int outW = rgbWriter.getWidth();
    const int outH = rgbWriter.getHeight();

    // ---- Decode -> two RGBA buffers ---------------------------------------
    AVFrame* frame = av_frame_alloc();
    AVFrame* rgba  = av_frame_alloc();
    AVPacket* pkt  = av_packet_alloc();

    SwsContext* sws = sws_getContext (w, h, dec->pix_fmt, outW, outH, AV_PIX_FMT_RGBA,
                                      SWS_BILINEAR, nullptr, nullptr, nullptr);

    bool allocated = false;

    if (frame == nullptr || rgba == nullptr || pkt == nullptr || sws == nullptr
        || av_image_alloc (rgba->data, rgba->linesize, outW, outH, AV_PIX_FMT_RGBA, 1) < 0)
    {
        error = "Out of memory setting up the split.";
    }
    else
    {
        allocated = true;
    }

    std::vector<juce::uint8> colourBuf, alphaBuf;
    const size_t rowBytes = (size_t) outW * 4;

    if (allocated)
    {
        colourBuf.resize (rowBytes * (size_t) outH);
        alphaBuf .resize (rowBytes * (size_t) outH);
    }

    bool success = allocated;
    juce::int64 frameIndex = 0;
    bool draining = false;

    auto handleFrame = [&] () -> bool
    {
        sws_scale (sws, frame->data, frame->linesize, 0, h, rgba->data, rgba->linesize);

        const size_t stride = (size_t) rgba->linesize[0];

        for (size_t y = 0; y < (size_t) outH; ++y)
        {
            const auto* src = rgba->data[0] + y * stride;
            auto* c = colourBuf.data() + y * rowBytes;
            auto* a = alphaBuf.data()  + y * rowBytes;

            for (size_t x = 0; x < (size_t) outW; ++x)
            {
                const auto* s = src + x * 4;
                const juce::uint8 av = s[3];

                if (current.unpremultiply && av > 0)
                {
                    // Undo HapEncoder's premultiply: c / a, rounded and clamped.
                    for (int i = 0; i < 3; ++i)
                        c[x * 4 + i] = (juce::uint8)
                            juce::jlimit (0, 255, (s[i] * 255 + av / 2) / av);
                }
                else if (current.unpremultiply)
                {
                    // Fully transparent: no colour to recover.
                    c[x * 4 + 0] = c[x * 4 + 1] = c[x * 4 + 2] = 0;
                }
                else
                {
                    c[x * 4 + 0] = s[0];
                    c[x * 4 + 1] = s[1];
                    c[x * 4 + 2] = s[2];
                }

                c[x * 4 + 3] = 255;

                // Alpha as neutral grey.
                a[x * 4 + 0] = a[x * 4 + 1] = a[x * 4 + 2] = av;
                a[x * 4 + 3] = 255;
            }
        }

        if (! rgbWriter.writeFrame (colourBuf.data(), (int) rowBytes, frameIndex)
            || ! alphaWriter.writeFrame (alphaBuf.data(), (int) rowBytes, frameIndex))
        {
            error = "Could not write an output frame.";
            return false;
        }

        ++frameIndex;

        if (onProgress != nullptr && ((frameIndex % 4) == 0 || frameIndex == 1))
        {
            const double p = expectedFrames > 0
                               ? juce::jlimit (0.0, 0.999,
                                     (double) frameIndex / (double) expectedFrames)
                               : -1.0;

            juce::MessageManager::callAsync ([cb = onProgress, p] { cb (p); });
        }

        return true;
    };

    while (success && ! threadShouldExit())
    {
        const int ret = avcodec_receive_frame (dec, frame);

        if (ret == 0)
        {
            if (! handleFrame())
                success = false;

            continue;
        }

        if (ret == AVERROR_EOF)
            break;

        if (ret != AVERROR (EAGAIN))
        {
            error = "Decode error.";
            success = false;
            break;
        }

        if (draining)
            break;

        if (av_read_frame (fmt, pkt) < 0)
        {
            avcodec_send_packet (dec, nullptr);
            draining = true;
            continue;
        }

        if (pkt->stream_index == streamIndex)
            avcodec_send_packet (dec, pkt);

        av_packet_unref (pkt);
    }

    if (success && ! threadShouldExit())
    {
        if (! rgbWriter.finish() || ! alphaWriter.finish())
        {
            error = "Could not finalise the output files.";
            success = false;
        }
    }
    else if (success)
    {
        success = false;   // cancelled
    }

    if (success && frameIndex == 0)
    {
        error = "No frames could be read from the source.";
        success = false;
    }

    if (allocated) av_freep (&rgba->data[0]);
    if (sws)   sws_freeContext (sws);
    if (pkt)   av_packet_free (&pkt);
    if (rgba)  av_frame_free (&rgba);
    if (frame) av_frame_free (&frame);
    avcodec_free_context (&dec);

    rgbWriter.close();
    alphaWriter.close();

    if (! success)
    {
        rgbFile.deleteFile();
        alphaFile.deleteFile();
    }
    else if (onProgress != nullptr)
    {
        juce::MessageManager::callAsync ([cb = onProgress] { cb (1.0); });
    }

    return success;
}
