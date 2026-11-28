#pragma once
// =============================================================================
// ffmpeg_common.h
//
// Internal helpers shared by video.cpp / audio.cpp. NOT part of the public
// include tree — only the .cpp files that talk to FFmpeg include this header.
//
// Provides:
//   * extern "C" inclusion of the libav* headers
//   * RAII wrappers (unique_ptr + custom deleters) for the FFmpeg handle types
//   * av_err_to_string() error-code formatting
//
// All declarations are guarded by CORONA_RESOURCE_HAVE_FFMPEG so the file is a
// no-op when FFmpeg is unavailable.
// =============================================================================

#ifdef CORONA_RESOURCE_HAVE_FFMPEG

#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace Corona::Resource::ffmpeg {

// -----------------------------------------------------------------------------
// Error formatting
// -----------------------------------------------------------------------------
inline std::string av_err_to_string(int errnum) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errnum, buf, sizeof(buf));
    return std::string(buf);
}

// -----------------------------------------------------------------------------
// RAII deleters
// -----------------------------------------------------------------------------
struct FormatContextInputDeleter {
    void operator()(AVFormatContext* ctx) const {
        if (ctx) {
            avformat_close_input(&ctx);
        }
    }
};

struct FormatContextOutputDeleter {
    void operator()(AVFormatContext* ctx) const {
        if (ctx) {
            // Close the underlying IO if it was opened and not AVFMT_NOFILE.
            if (ctx->pb && !(ctx->oformat->flags & AVFMT_NOFILE)) {
                avio_closep(&ctx->pb);
            }
            avformat_free_context(ctx);
        }
    }
};

struct CodecContextDeleter {
    void operator()(AVCodecContext* ctx) const {
        if (ctx) {
            avcodec_free_context(&ctx);
        }
    }
};

struct FrameDeleter {
    void operator()(AVFrame* frame) const {
        if (frame) {
            av_frame_free(&frame);
        }
    }
};

struct PacketDeleter {
    void operator()(AVPacket* pkt) const {
        if (pkt) {
            av_packet_free(&pkt);
        }
    }
};

struct SwsContextDeleter {
    void operator()(SwsContext* ctx) const {
        if (ctx) {
            sws_freeContext(ctx);
        }
    }
};

struct SwrContextDeleter {
    void operator()(SwrContext* ctx) const {
        if (ctx) {
            swr_free(&ctx);
        }
    }
};

// -----------------------------------------------------------------------------
// Smart-pointer aliases
// -----------------------------------------------------------------------------
using InputFormatContext = std::unique_ptr<AVFormatContext, FormatContextInputDeleter>;
using OutputFormatContext = std::unique_ptr<AVFormatContext, FormatContextOutputDeleter>;
using CodecContext = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using Frame = std::unique_ptr<AVFrame, FrameDeleter>;
using Packet = std::unique_ptr<AVPacket, PacketDeleter>;
using SwsCtx = std::unique_ptr<SwsContext, SwsContextDeleter>;
using SwrCtx = std::unique_ptr<SwrContext, SwrContextDeleter>;

// -----------------------------------------------------------------------------
// Factory helpers
// -----------------------------------------------------------------------------
inline Frame make_frame() {
    return Frame(av_frame_alloc());
}

inline Packet make_packet() {
    return Packet(av_packet_alloc());
}

}  // namespace Corona::Resource::ffmpeg

#endif  // CORONA_RESOURCE_HAVE_FFMPEG
