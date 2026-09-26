#include "corona/resource/types/video.h"

#include "horizon/core/logging.h"

#ifdef CORONA_RESOURCE_HAVE_FFMPEG
#include "ffmpeg_common.h"
#endif

namespace Corona::Resource {

#ifdef CORONA_RESOURCE_HAVE_FFMPEG

using namespace Corona::Resource::ffmpeg;

// =============================================================================
// Video::Decoder — owns the FFmpeg demux/decode/scale state for one video.
// =============================================================================
struct Video::Decoder {
    InputFormatContext fmt;
    CodecContext codec;
    SwsCtx sws;
    Frame frame = make_frame();
    Frame rgba_frame = make_frame();
    Packet packet = make_packet();
    int stream_index = -1;
    AVRational time_base{0, 1};
    int sws_w = 0, sws_h = 0;  // dimensions the sws context was built for

    // Convert the currently decoded `frame` into an RGBA VideoFrame.
    std::optional<VideoFrame> to_rgba() {
        const int w = codec->width;
        const int h = codec->height;

        // (Re)build the scaler if the source dimensions changed.
        if (!sws || sws_w != w || sws_h != h) {
            sws.reset(sws_getContext(w, h, codec->pix_fmt,
                                     w, h, AV_PIX_FMT_RGBA,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr));
            if (!sws) {
                CFW_LOG_ERROR("[Video] Failed to create sws scaler context");
                return std::nullopt;
            }
            sws_w = w;
            sws_h = h;
        }

        VideoFrame out;
        out.width = w;
        out.height = h;
        out.rgba.resize(static_cast<size_t>(w) * h * 4);

        uint8_t* dst[4] = {out.rgba.data(), nullptr, nullptr, nullptr};
        int dst_linesize[4] = {w * 4, 0, 0, 0};
        sws_scale(sws.get(), frame->data, frame->linesize, 0, h, dst, dst_linesize);

        const int64_t pts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                                ? frame->best_effort_timestamp
                                : frame->pts;
        if (pts != AV_NOPTS_VALUE && time_base.den != 0) {
            out.pts_seconds = static_cast<double>(pts) * av_q2d(time_base);
        }
        return out;
    }
};

#else   // !CORONA_RESOURCE_HAVE_FFMPEG

struct Video::Decoder {};

#endif  // CORONA_RESOURCE_HAVE_FFMPEG

Video::Video(const std::filesystem::path& path) : IResource(path), source_path_(path) {}

Video::~Video() = default;

bool Video::is_open() const {
    return decoder_ != nullptr;
}

#ifdef CORONA_RESOURCE_HAVE_FFMPEG

std::optional<VideoFrame> Video::decode_next_frame() {
    if (!decoder_) {
        return std::nullopt;
    }
    auto& d = *decoder_;

    while (true) {
        // Drain frames already buffered in the decoder first.
        int ret = avcodec_receive_frame(d.codec.get(), d.frame.get());
        if (ret == 0) {
            return d.to_rgba();
        }
        if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
            CFW_LOG_ERROR("[Video] avcodec_receive_frame failed: {}", av_err_to_string(ret));
            return std::nullopt;
        }

        // Need more input: read the next packet from the video stream.
        av_packet_unref(d.packet.get());
        ret = av_read_frame(d.fmt.get(), d.packet.get());
        if (ret < 0) {
            // End of file: flush the decoder once.
            avcodec_send_packet(d.codec.get(), nullptr);
            ret = avcodec_receive_frame(d.codec.get(), d.frame.get());
            if (ret == 0) {
                return d.to_rgba();
            }
            return std::nullopt;
        }

        if (d.packet->stream_index != d.stream_index) {
            continue;  // not our video stream
        }

        ret = avcodec_send_packet(d.codec.get(), d.packet.get());
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            CFW_LOG_ERROR("[Video] avcodec_send_packet failed: {}", av_err_to_string(ret));
            return std::nullopt;
        }
    }
}

bool Video::seek(double seconds) {
    if (!decoder_) {
        return false;
    }
    auto& d = *decoder_;

    const int64_t ts = (d.time_base.num != 0)
                           ? static_cast<int64_t>(seconds / av_q2d(d.time_base))
                           : static_cast<int64_t>(seconds * AV_TIME_BASE);

    int ret = av_seek_frame(d.fmt.get(), d.stream_index, ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        CFW_LOG_ERROR("[Video] seek to {}s failed: {}", seconds, av_err_to_string(ret));
        return false;
    }
    avcodec_flush_buffers(d.codec.get());
    return true;
}

std::optional<VideoFrame> Video::decode_frame_at(double seconds) {
    if (!seek(seconds)) {
        return std::nullopt;
    }
    return decode_next_frame();
}

void Video::reset() {
    seek(0.0);
}

#else   // !CORONA_RESOURCE_HAVE_FFMPEG

std::optional<VideoFrame> Video::decode_next_frame() { return std::nullopt; }
bool Video::seek(double) { return false; }
std::optional<VideoFrame> Video::decode_frame_at(double) { return std::nullopt; }
void Video::reset() {}

#endif  // CORONA_RESOURCE_HAVE_FFMPEG

// =============================================================================
// VideoParser
// =============================================================================
VideoParser::VideoParser() {
#ifdef CORONA_RESOURCE_HAVE_FFMPEG
    const char* import_exts[] = {".mp4", ".avi", ".mkv", ".mov", ".webm", ".flv"};
    for (const auto* ext : import_exts) {
        register_extension(ext, [this](const auto& path, ResourceCache&) { return parse_video(path); });
    }

    const char* export_exts[] = {".mp4", ".mkv", ".mov", ".webm"};
    for (const auto* ext : export_exts) {
        register_exporter(ext, [this](const IResource& r, const std::filesystem::path& p) { return export_video(r, p); });
    }
#else
    CFW_LOG_WARNING("[VideoParser] Built without FFmpeg; video import/export disabled");
#endif
}

#ifdef CORONA_RESOURCE_HAVE_FFMPEG

std::shared_ptr<IResource> VideoParser::parse_video(const std::filesystem::path& path) {
    auto video = std::make_shared<Video>(path);
    auto decoder = std::make_unique<Video::Decoder>();

    // Open the container.
    AVFormatContext* raw_fmt = nullptr;
    int ret = avformat_open_input(&raw_fmt, path.string().c_str(), nullptr, nullptr);
    if (ret < 0) {
        CFW_LOG_ERROR("[VideoParser] Cannot open '{}': {}", path.string(), av_err_to_string(ret));
        return nullptr;
    }
    decoder->fmt.reset(raw_fmt);

    if (avformat_find_stream_info(decoder->fmt.get(), nullptr) < 0) {
        CFW_LOG_ERROR("[VideoParser] Cannot read stream info from '{}'", path.string());
        return nullptr;
    }

    // Find the best video stream.
    const AVCodec* codec = nullptr;
    decoder->stream_index = av_find_best_stream(decoder->fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (decoder->stream_index < 0 || !codec) {
        CFW_LOG_ERROR("[VideoParser] No video stream in '{}'", path.string());
        return nullptr;
    }

    AVStream* stream = decoder->fmt->streams[decoder->stream_index];
    decoder->time_base = stream->time_base;

    // Set up the decoder context.
    decoder->codec.reset(avcodec_alloc_context3(codec));
    if (!decoder->codec) {
        CFW_LOG_ERROR("[VideoParser] Failed to alloc codec context");
        return nullptr;
    }
    if (avcodec_parameters_to_context(decoder->codec.get(), stream->codecpar) < 0) {
        CFW_LOG_ERROR("[VideoParser] Failed to copy codec parameters");
        return nullptr;
    }
    if (avcodec_open2(decoder->codec.get(), codec, nullptr) < 0) {
        CFW_LOG_ERROR("[VideoParser] Failed to open codec");
        return nullptr;
    }

    // Fill metadata.
    VideoMetadata& m = video->meta_;
    m.width = decoder->codec->width;
    m.height = decoder->codec->height;
    m.codec_name = codec->name ? codec->name : "";
    m.bit_rate = decoder->fmt->bit_rate;
    m.frame_count = stream->nb_frames;
    if (stream->avg_frame_rate.den != 0) {
        m.fps = av_q2d(stream->avg_frame_rate);
    }
    if (decoder->fmt->duration != AV_NOPTS_VALUE) {
        m.duration_seconds = static_cast<double>(decoder->fmt->duration) / AV_TIME_BASE;
    } else if (stream->duration != AV_NOPTS_VALUE && stream->time_base.den != 0) {
        m.duration_seconds = static_cast<double>(stream->duration) * av_q2d(stream->time_base);
    }
    if (m.frame_count == 0 && m.fps > 0.0 && m.duration_seconds > 0.0) {
        m.frame_count = static_cast<int64_t>(m.fps * m.duration_seconds);
    }

    video->decoder_ = std::move(decoder);

    CFW_LOG_DEBUG("[VideoParser] Loaded '{}' | {}x{} | {} | {:.2f}s | {:.2f}fps",
                  path.string(), m.width, m.height, m.codec_name, m.duration_seconds, m.fps);
    return video;
}

#endif  // CORONA_RESOURCE_HAVE_FFMPEG

#ifdef CORONA_RESOURCE_HAVE_FFMPEG

bool VideoParser::export_video(const IResource& resource, const std::filesystem::path& path) {
    // We transcode from the original source file (frames are not kept in memory).
    const auto* video = dynamic_cast<const Video*>(&resource);
    if (!video) {
        CFW_LOG_ERROR("[VideoParser] export target is not a Video resource");
        return false;
    }
    const std::filesystem::path& src = video->source_path();
    if (src.empty() || !std::filesystem::exists(src)) {
        CFW_LOG_ERROR("[VideoParser] source file unavailable for transcode: '{}'", src.string());
        return false;
    }

    // ---- Open input + decoder -------------------------------------------------
    InputFormatContext in_fmt;
    {
        AVFormatContext* raw = nullptr;
        if (avformat_open_input(&raw, src.string().c_str(), nullptr, nullptr) < 0) {
            CFW_LOG_ERROR("[VideoParser] transcode: cannot open input '{}'", src.string());
            return false;
        }
        in_fmt.reset(raw);
    }
    if (avformat_find_stream_info(in_fmt.get(), nullptr) < 0) {
        CFW_LOG_ERROR("[VideoParser] transcode: cannot read stream info");
        return false;
    }

    const AVCodec* dec_codec = nullptr;
    int in_stream_idx = av_find_best_stream(in_fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &dec_codec, 0);
    if (in_stream_idx < 0 || !dec_codec) {
        CFW_LOG_ERROR("[VideoParser] transcode: no video stream");
        return false;
    }
    AVStream* in_stream = in_fmt->streams[in_stream_idx];

    CodecContext dec;
    dec.reset(avcodec_alloc_context3(dec_codec));
    avcodec_parameters_to_context(dec.get(), in_stream->codecpar);
    if (avcodec_open2(dec.get(), dec_codec, nullptr) < 0) {
        CFW_LOG_ERROR("[VideoParser] transcode: cannot open decoder");
        return false;
    }

    // ---- Open output container + encoder -------------------------------------
    OutputFormatContext out_fmt;
    {
        AVFormatContext* raw = nullptr;
        avformat_alloc_output_context2(&raw, nullptr, nullptr, path.string().c_str());
        if (!raw) {
            CFW_LOG_ERROR("[VideoParser] transcode: cannot create output context for '{}'", path.string());
            return false;
        }
        out_fmt.reset(raw);
    }

    const AVCodec* enc_codec = avcodec_find_encoder(out_fmt->oformat->video_codec);
    if (!enc_codec) {
        CFW_LOG_ERROR("[VideoParser] transcode: no encoder for output format");
        return false;
    }

    AVStream* out_stream = avformat_new_stream(out_fmt.get(), nullptr);
    if (!out_stream) {
        CFW_LOG_ERROR("[VideoParser] transcode: cannot create output stream");
        return false;
    }

    CodecContext enc;
    enc.reset(avcodec_alloc_context3(enc_codec));

    const int width = dec->width;
    const int height = dec->height;
    AVRational fps = in_stream->avg_frame_rate;
    if (fps.num == 0 || fps.den == 0) {
        fps = AVRational{25, 1};
    }

    enc->width = width;
    enc->height = height;
    enc->time_base = AVRational{fps.den, fps.num};
    enc->framerate = fps;
    enc->gop_size = 12;
    enc->max_b_frames = 2;
    enc->bit_rate = (video->metadata().bit_rate > 0) ? video->metadata().bit_rate : 4'000'000;

    // Pick a pixel format the encoder supports (default YUV420P).
    enc->pix_fmt = (enc_codec->pix_fmts) ? enc_codec->pix_fmts[0] : AV_PIX_FMT_YUV420P;

    if (out_fmt->oformat->flags & AVFMT_GLOBALHEADER) {
        enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    if (avcodec_open2(enc.get(), enc_codec, nullptr) < 0) {
        CFW_LOG_ERROR("[VideoParser] transcode: cannot open encoder");
        return false;
    }
    avcodec_parameters_from_context(out_stream->codecpar, enc.get());
    out_stream->time_base = enc->time_base;

    // ---- Open output file ----------------------------------------------------
    if (!(out_fmt->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&out_fmt->pb, path.string().c_str(), AVIO_FLAG_WRITE) < 0) {
            CFW_LOG_ERROR("[VideoParser] transcode: cannot open output file '{}'", path.string());
            return false;
        }
    }
    if (avformat_write_header(out_fmt.get(), nullptr) < 0) {
        CFW_LOG_ERROR("[VideoParser] transcode: cannot write header");
        return false;
    }

    // ---- Scaler: decoder pix_fmt -> encoder pix_fmt --------------------------
    SwsCtx sws(sws_getContext(width, height, dec->pix_fmt,
                              width, height, enc->pix_fmt,
                              SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!sws) {
        CFW_LOG_ERROR("[VideoParser] transcode: cannot create scaler");
        return false;
    }

    Frame dec_frame = make_frame();
    Frame enc_frame = make_frame();
    enc_frame->format = enc->pix_fmt;
    enc_frame->width = width;
    enc_frame->height = height;
    if (av_frame_get_buffer(enc_frame.get(), 0) < 0) {
        CFW_LOG_ERROR("[VideoParser] transcode: cannot alloc frame buffer");
        return false;
    }

    Packet in_pkt = make_packet();
    Packet out_pkt = make_packet();
    int64_t next_pts = 0;
    bool ok = true;

    // Encode a single (possibly null = flush) frame and mux the output packets.
    auto encode_and_write = [&](AVFrame* frame) -> bool {
        int ret = avcodec_send_frame(enc.get(), frame);
        if (ret < 0) {
            CFW_LOG_ERROR("[VideoParser] transcode: send_frame failed: {}", av_err_to_string(ret));
            return false;
        }
        while (ret >= 0) {
            ret = avcodec_receive_packet(enc.get(), out_pkt.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                CFW_LOG_ERROR("[VideoParser] transcode: receive_packet failed: {}", av_err_to_string(ret));
                return false;
            }
            av_packet_rescale_ts(out_pkt.get(), enc->time_base, out_stream->time_base);
            out_pkt->stream_index = out_stream->index;
            ret = av_interleaved_write_frame(out_fmt.get(), out_pkt.get());
            av_packet_unref(out_pkt.get());
            if (ret < 0) {
                CFW_LOG_ERROR("[VideoParser] transcode: write_frame failed: {}", av_err_to_string(ret));
                return false;
            }
        }
        return true;
    };

    // ---- Main demux/decode/scale/encode loop ---------------------------------
    while (ok) {
        int ret = av_read_frame(in_fmt.get(), in_pkt.get());
        if (ret < 0) {
            break;  // EOF
        }
        if (in_pkt->stream_index != in_stream_idx) {
            av_packet_unref(in_pkt.get());
            continue;
        }

        ret = avcodec_send_packet(dec.get(), in_pkt.get());
        av_packet_unref(in_pkt.get());
        if (ret < 0) {
            CFW_LOG_ERROR("[VideoParser] transcode: decoder send_packet failed: {}", av_err_to_string(ret));
            ok = false;
            break;
        }

        while (ret >= 0) {
            ret = avcodec_receive_frame(dec.get(), dec_frame.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                ok = false;
                break;
            }
            if (av_frame_make_writable(enc_frame.get()) < 0) {
                ok = false;
                break;
            }
            sws_scale(sws.get(), dec_frame->data, dec_frame->linesize, 0, height,
                      enc_frame->data, enc_frame->linesize);
            enc_frame->pts = next_pts++;
            if (!encode_and_write(enc_frame.get())) {
                ok = false;
                break;
            }
        }
    }

    // ---- Flush decoder then encoder ------------------------------------------
    if (ok) {
        avcodec_send_packet(dec.get(), nullptr);
        int ret = 0;
        while (ret >= 0) {
            ret = avcodec_receive_frame(dec.get(), dec_frame.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                break;
            }
            if (av_frame_make_writable(enc_frame.get()) < 0) {
                break;
            }
            sws_scale(sws.get(), dec_frame->data, dec_frame->linesize, 0, height,
                      enc_frame->data, enc_frame->linesize);
            enc_frame->pts = next_pts++;
            if (!encode_and_write(enc_frame.get())) {
                ok = false;
                break;
            }
        }
        encode_and_write(nullptr);  // flush encoder
    }

    av_write_trailer(out_fmt.get());

    if (ok) {
        CFW_LOG_DEBUG("[VideoParser] Transcoded '{}' -> '{}'", src.string(), path.string());
    }
    return ok;
}

#else   // !CORONA_RESOURCE_HAVE_FFMPEG

std::shared_ptr<IResource> VideoParser::parse_video(const std::filesystem::path&) { return nullptr; }
bool VideoParser::export_video(const IResource&, const std::filesystem::path&) { return false; }

#endif  // CORONA_RESOURCE_HAVE_FFMPEG

}  // namespace Corona::Resource
