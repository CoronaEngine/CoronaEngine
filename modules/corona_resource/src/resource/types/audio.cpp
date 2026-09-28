#include "corona/resource/types/audio.h"

#include <algorithm>

#include "horizon/core/logging.h"

#ifdef CORONA_RESOURCE_HAVE_MINIAUDIO
#include <miniaudio.h>
#endif

#ifdef CORONA_RESOURCE_HAVE_FFMPEG
#include "ffmpeg_common.h"
#endif

namespace Corona::Resource {

#ifdef CORONA_RESOURCE_HAVE_FFMPEG
using namespace Corona::Resource::ffmpeg;
#endif

Audio::Audio(const std::filesystem::path& path) : IResource(path) {}

void Audio::set_samples(std::vector<float> samples, int sample_rate, int channels) {
    pcm_ = std::move(samples);
    meta_.sample_rate = sample_rate;
    meta_.channels = channels;
    meta_.frame_count = (channels > 0) ? static_cast<std::int64_t>(pcm_.size() / channels) : 0;
    meta_.duration_seconds = (sample_rate > 0)
                                 ? static_cast<double>(meta_.frame_count) / sample_rate
                                 : 0.0;
}

// =============================================================================
// AudioParser
// =============================================================================
AudioParser::AudioParser() {
    // Import is backed by miniaudio (built-in wav/mp3/flac/ogg decoders).
#ifdef CORONA_RESOURCE_HAVE_MINIAUDIO
    const char* import_exts[] = {".wav", ".mp3", ".ogg", ".flac"};
    for (const auto* ext : import_exts) {
        register_extension(ext, [this](const auto& path, ResourceCache&) { return parse_audio(path); });
    }
#else
    CFW_LOG_WARNING("[AudioParser] Built without miniaudio; audio import disabled");
#endif

    // Export is still backed by FFmpeg (miniaudio only encodes WAV).
#ifdef CORONA_RESOURCE_HAVE_FFMPEG
    // Import formats miniaudio has no built-in decoder for, routed through FFmpeg.
    const char* ffmpeg_import_exts[] = {".aac", ".m4a"};
    for (const auto* ext : ffmpeg_import_exts) {
        register_extension(ext, [this](const auto& path, ResourceCache&) { return parse_audio_ffmpeg(path); });
    }

    const char* export_exts[] = {".wav", ".flac", ".ogg", ".aac", ".mp3"};
    for (const auto* ext : export_exts) {
        register_exporter(ext, [this](const IResource& r, const std::filesystem::path& p) { return export_audio(r, p); });
    }
#else
    CFW_LOG_WARNING("[AudioParser] Built without FFmpeg; audio export disabled");
#endif
}

#ifdef CORONA_RESOURCE_HAVE_MINIAUDIO

std::shared_ptr<IResource> AudioParser::parse_audio(const std::filesystem::path& path) {
    // Decode the whole file to interleaved float32 PCM, preserving the source
    // channel count / sample rate (pass 0 to let miniaudio keep the native values).
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);

    ma_decoder decoder;
    ma_result res = ma_decoder_init_file(path.string().c_str(), &config, &decoder);
    if (res != MA_SUCCESS) {
        CFW_LOG_ERROR("[AudioParser] Cannot open '{}': miniaudio error {}", path.string(), static_cast<int>(res));
        return nullptr;
    }

    const int out_channels = static_cast<int>(decoder.outputChannels);
    const int out_rate = static_cast<int>(decoder.outputSampleRate);

    ma_uint64 total_frames = 0;
    res = ma_decoder_get_length_in_pcm_frames(&decoder, &total_frames);
    if (res != MA_SUCCESS || total_frames == 0) {
        // Length may be unknown for some streams; fall back to chunked reads.
        std::vector<float> pcm;
        const ma_uint64 chunk_frames = 4096;
        std::vector<float> chunk(static_cast<size_t>(chunk_frames) * out_channels);
        for (;;) {
            ma_uint64 read = 0;
            ma_result rr = ma_decoder_read_pcm_frames(&decoder, chunk.data(), chunk_frames, &read);
            if (read > 0) {
                pcm.insert(pcm.end(), chunk.begin(),
                           chunk.begin() + static_cast<size_t>(read) * out_channels);
            }
            if (rr != MA_SUCCESS || read < chunk_frames) {
                break;
            }
        }
        ma_decoder_uninit(&decoder);

        if (pcm.empty()) {
            CFW_LOG_ERROR("[AudioParser] Decoded no PCM from '{}'", path.string());
            return nullptr;
        }
        auto audio = std::make_shared<Audio>(path);
        audio->set_samples(std::move(pcm), out_rate, out_channels);

        const auto& m = audio->metadata();
        CFW_LOG_DEBUG("[AudioParser] Loaded '{}' | {}Hz | {}ch | {:.2f}s (streamed)",
                      path.string(), m.sample_rate, m.channels, m.duration_seconds);
        return audio;
    }

    std::vector<float> pcm(static_cast<size_t>(total_frames) * out_channels);
    ma_uint64 frames_read = 0;
    res = ma_decoder_read_pcm_frames(&decoder, pcm.data(), total_frames, &frames_read);
    ma_decoder_uninit(&decoder);

    if (res != MA_SUCCESS && frames_read == 0) {
        CFW_LOG_ERROR("[AudioParser] Failed to read PCM from '{}': miniaudio error {}",
                      path.string(), static_cast<int>(res));
        return nullptr;
    }
    // Shrink to what was actually decoded.
    pcm.resize(static_cast<size_t>(frames_read) * out_channels);

    auto audio = std::make_shared<Audio>(path);
    audio->set_samples(std::move(pcm), out_rate, out_channels);

    const auto& m = audio->metadata();
    CFW_LOG_DEBUG("[AudioParser] Loaded '{}' | {}Hz | {}ch | {:.2f}s",
                  path.string(), m.sample_rate, m.channels, m.duration_seconds);
    return audio;
}

#else  // !CORONA_RESOURCE_HAVE_MINIAUDIO

std::shared_ptr<IResource> AudioParser::parse_audio(const std::filesystem::path&) { return nullptr; }

#endif  // CORONA_RESOURCE_HAVE_MINIAUDIO

#ifdef CORONA_RESOURCE_HAVE_FFMPEG

std::shared_ptr<IResource> AudioParser::parse_audio_ffmpeg(const std::filesystem::path& path) {
    // ---- Open container + find audio stream ----------------------------------
    InputFormatContext fmt;
    {
        AVFormatContext* raw = nullptr;
        int ret = avformat_open_input(&raw, path.string().c_str(), nullptr, nullptr);
        if (ret < 0) {
            CFW_LOG_ERROR("[AudioParser] Cannot open '{}': {}", path.string(), av_err_to_string(ret));
            return nullptr;
        }
        fmt.reset(raw);
    }
    if (avformat_find_stream_info(fmt.get(), nullptr) < 0) {
        CFW_LOG_ERROR("[AudioParser] Cannot read stream info from '{}'", path.string());
        return nullptr;
    }

    const AVCodec* codec = nullptr;
    int stream_idx = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (stream_idx < 0 || !codec) {
        CFW_LOG_ERROR("[AudioParser] No audio stream in '{}'", path.string());
        return nullptr;
    }
    AVStream* stream = fmt->streams[stream_idx];

    // ---- Decoder -------------------------------------------------------------
    CodecContext dec;
    dec.reset(avcodec_alloc_context3(codec));
    if (!dec || avcodec_parameters_to_context(dec.get(), stream->codecpar) < 0) {
        CFW_LOG_ERROR("[AudioParser] Failed to set up decoder");
        return nullptr;
    }
    if (avcodec_open2(dec.get(), codec, nullptr) < 0) {
        CFW_LOG_ERROR("[AudioParser] Failed to open decoder");
        return nullptr;
    }

    const int out_channels = dec->ch_layout.nb_channels;
    const int out_rate = dec->sample_rate;

    // ---- Resampler: any input -> interleaved float32 -------------------------
    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, out_channels);

    SwrCtx swr;
    {
        SwrContext* raw = nullptr;
        int ret = swr_alloc_set_opts2(&raw,
                                      &out_layout, AV_SAMPLE_FMT_FLT, out_rate,
                                      &dec->ch_layout, dec->sample_fmt, dec->sample_rate,
                                      0, nullptr);
        if (ret < 0 || !raw) {
            CFW_LOG_ERROR("[AudioParser] Failed to alloc resampler: {}", av_err_to_string(ret));
            av_channel_layout_uninit(&out_layout);
            return nullptr;
        }
        swr.reset(raw);
    }
    if (swr_init(swr.get()) < 0) {
        CFW_LOG_ERROR("[AudioParser] Failed to init resampler");
        av_channel_layout_uninit(&out_layout);
        return nullptr;
    }

    // ---- Decode + resample loop ----------------------------------------------
    std::vector<float> pcm;
    Frame frame = make_frame();
    Packet packet = make_packet();

    auto drain_resampler_tail = [&]() {
        // Flush any samples buffered inside swr.
        for (;;) {
            int max_out = swr_get_out_samples(swr.get(), 0);
            if (max_out <= 0) {
                break;
            }
            std::vector<float> buf(static_cast<size_t>(max_out) * out_channels);
            uint8_t* out_ptr[1] = {reinterpret_cast<uint8_t*>(buf.data())};
            int got = swr_convert(swr.get(), out_ptr, max_out, nullptr, 0);
            if (got <= 0) {
                break;
            }
            pcm.insert(pcm.end(), buf.begin(), buf.begin() + static_cast<size_t>(got) * out_channels);
        }
    };

    auto handle_frame = [&]() {
        int max_out = swr_get_out_samples(swr.get(), frame->nb_samples);
        if (max_out <= 0) {
            return;
        }
        std::vector<float> buf(static_cast<size_t>(max_out) * out_channels);
        uint8_t* out_ptr[1] = {reinterpret_cast<uint8_t*>(buf.data())};
        int got = swr_convert(swr.get(), out_ptr, max_out,
                              const_cast<const uint8_t**>(frame->data), frame->nb_samples);
        if (got > 0) {
            pcm.insert(pcm.end(), buf.begin(), buf.begin() + static_cast<size_t>(got) * out_channels);
        }
    };

    while (av_read_frame(fmt.get(), packet.get()) >= 0) {
        if (packet->stream_index == stream_idx) {
            int ret = avcodec_send_packet(dec.get(), packet.get());
            if (ret >= 0) {
                while (avcodec_receive_frame(dec.get(), frame.get()) >= 0) {
                    handle_frame();
                }
            }
        }
        av_packet_unref(packet.get());
    }
    // Flush decoder.
    avcodec_send_packet(dec.get(), nullptr);
    while (avcodec_receive_frame(dec.get(), frame.get()) >= 0) {
        handle_frame();
    }
    drain_resampler_tail();

    av_channel_layout_uninit(&out_layout);

    if (pcm.empty()) {
        CFW_LOG_ERROR("[AudioParser] Decoded no PCM from '{}'", path.string());
        return nullptr;
    }

    auto audio = std::make_shared<Audio>(path);
    audio->set_samples(std::move(pcm), out_rate, out_channels);
    audio->meta_.codec_name = codec->name ? codec->name : "";

    const auto& m = audio->metadata();
    CFW_LOG_DEBUG("[AudioParser] Loaded '{}' | {}Hz | {}ch | {:.2f}s | {} (ffmpeg)",
                  path.string(), m.sample_rate, m.channels, m.duration_seconds, m.codec_name);
    return audio;
}

bool AudioParser::export_audio(const IResource& resource, const std::filesystem::path& path) {
    const auto* audio = dynamic_cast<const Audio*>(&resource);
    if (!audio) {
        CFW_LOG_ERROR("[AudioParser] export target is not an Audio resource");
        return false;
    }
    const auto& meta = audio->metadata();
    const std::vector<float>& pcm = audio->samples();
    if (pcm.empty() || meta.channels <= 0 || meta.sample_rate <= 0) {
        CFW_LOG_ERROR("[AudioParser] export: empty/invalid PCM data");
        return false;
    }

    // ---- Output container ----------------------------------------------------
    OutputFormatContext out_fmt;
    {
        AVFormatContext* raw = nullptr;
        avformat_alloc_output_context2(&raw, nullptr, nullptr, path.string().c_str());
        if (!raw) {
            CFW_LOG_ERROR("[AudioParser] export: cannot create output context for '{}'", path.string());
            return false;
        }
        out_fmt.reset(raw);
    }

    const AVCodec* enc_codec = avcodec_find_encoder(out_fmt->oformat->audio_codec);
    if (!enc_codec) {
        CFW_LOG_ERROR("[AudioParser] export: no encoder available for '{}' (may be a non-LGPL codec)",
                      path.extension().string());
        return false;
    }

    AVStream* out_stream = avformat_new_stream(out_fmt.get(), nullptr);
    if (!out_stream) {
        return false;
    }

    CodecContext enc;
    enc.reset(avcodec_alloc_context3(enc_codec));

    // Choose a sample format the encoder supports (prefer FLTP, else first).
    enc->sample_fmt = (enc_codec->sample_fmts) ? enc_codec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
    if (enc_codec->sample_fmts) {
        for (const enum AVSampleFormat* p = enc_codec->sample_fmts; *p != AV_SAMPLE_FMT_NONE; ++p) {
            if (*p == AV_SAMPLE_FMT_FLTP || *p == AV_SAMPLE_FMT_FLT) {
                enc->sample_fmt = *p;
                break;
            }
        }
    }
    enc->sample_rate = meta.sample_rate;
    av_channel_layout_default(&enc->ch_layout, meta.channels);
    enc->bit_rate = 192'000;
    enc->time_base = AVRational{1, meta.sample_rate};

    if (out_fmt->oformat->flags & AVFMT_GLOBALHEADER) {
        enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    if (avcodec_open2(enc.get(), enc_codec, nullptr) < 0) {
        CFW_LOG_ERROR("[AudioParser] export: cannot open encoder");
        return false;
    }
    avcodec_parameters_from_context(out_stream->codecpar, enc.get());
    out_stream->time_base = enc->time_base;

    // ---- Resampler: interleaved FLT (in memory) -> encoder format ------------
    AVChannelLayout in_layout;
    av_channel_layout_default(&in_layout, meta.channels);

    SwrCtx swr;
    {
        SwrContext* raw = nullptr;
        int ret = swr_alloc_set_opts2(&raw,
                                      &enc->ch_layout, enc->sample_fmt, enc->sample_rate,
                                      &in_layout, AV_SAMPLE_FMT_FLT, meta.sample_rate,
                                      0, nullptr);
        if (ret < 0 || !raw || swr_init(raw) < 0) {
            CFW_LOG_ERROR("[AudioParser] export: failed to set up resampler");
            av_channel_layout_uninit(&in_layout);
            if (raw) swr_free(&raw);
            return false;
        }
        swr.reset(raw);
    }

    // ---- Open file + header --------------------------------------------------
    if (!(out_fmt->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&out_fmt->pb, path.string().c_str(), AVIO_FLAG_WRITE) < 0) {
            CFW_LOG_ERROR("[AudioParser] export: cannot open output file '{}'", path.string());
            av_channel_layout_uninit(&in_layout);
            return false;
        }
    }
    if (avformat_write_header(out_fmt.get(), nullptr) < 0) {
        CFW_LOG_ERROR("[AudioParser] export: cannot write header");
        av_channel_layout_uninit(&in_layout);
        return false;
    }

    // Encoder frame size: codecs with VARIABLE_FRAME_SIZE report 0; pick a default.
    const int frame_size = (enc->frame_size > 0) ? enc->frame_size : 1024;

    Packet out_pkt = make_packet();
    int64_t pts = 0;
    bool ok = true;

    auto encode_and_write = [&](AVFrame* frame) -> bool {
        int ret = avcodec_send_frame(enc.get(), frame);
        if (ret < 0) {
            CFW_LOG_ERROR("[AudioParser] export: send_frame failed: {}", av_err_to_string(ret));
            return false;
        }
        while (ret >= 0) {
            ret = avcodec_receive_packet(enc.get(), out_pkt.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                return false;
            }
            av_packet_rescale_ts(out_pkt.get(), enc->time_base, out_stream->time_base);
            out_pkt->stream_index = out_stream->index;
            ret = av_interleaved_write_frame(out_fmt.get(), out_pkt.get());
            av_packet_unref(out_pkt.get());
            if (ret < 0) {
                return false;
            }
        }
        return true;
    };

    // Feed the interleaved float buffer through the resampler in frame_size chunks.
    const int64_t total_frames = meta.frame_count;
    int64_t pos = 0;  // per-channel sample position
    Frame enc_frame = make_frame();

    while (ok && pos < total_frames) {
        const int this_chunk = static_cast<int>(std::min<int64_t>(frame_size, total_frames - pos));

        const float* in_ptr = pcm.data() + pos * meta.channels;
        const uint8_t* in_data[1] = {reinterpret_cast<const uint8_t*>(in_ptr)};

        // Prepare the destination frame.
        av_frame_unref(enc_frame.get());
        enc_frame->nb_samples = frame_size;
        enc_frame->format = enc->sample_fmt;
        av_channel_layout_copy(&enc_frame->ch_layout, &enc->ch_layout);
        enc_frame->sample_rate = enc->sample_rate;
        if (av_frame_get_buffer(enc_frame.get(), 0) < 0) {
            ok = false;
            break;
        }

        int converted = swr_convert(swr.get(), enc_frame->data, frame_size, in_data, this_chunk);
        if (converted < 0) {
            ok = false;
            break;
        }
        if (converted == 0) {
            pos += this_chunk;
            continue;
        }
        enc_frame->nb_samples = converted;
        enc_frame->pts = pts;
        pts += converted;

        if (!encode_and_write(enc_frame.get())) {
            ok = false;
            break;
        }
        pos += this_chunk;
    }

    // Flush resampler tail then encoder.
    if (ok) {
        for (;;) {
            int remaining = swr_get_out_samples(swr.get(), 0);
            if (remaining <= 0) {
                break;
            }
            av_frame_unref(enc_frame.get());
            enc_frame->nb_samples = remaining;
            enc_frame->format = enc->sample_fmt;
            av_channel_layout_copy(&enc_frame->ch_layout, &enc->ch_layout);
            enc_frame->sample_rate = enc->sample_rate;
            if (av_frame_get_buffer(enc_frame.get(), 0) < 0) {
                break;
            }
            int converted = swr_convert(swr.get(), enc_frame->data, remaining, nullptr, 0);
            if (converted <= 0) {
                break;
            }
            enc_frame->nb_samples = converted;
            enc_frame->pts = pts;
            pts += converted;
            if (!encode_and_write(enc_frame.get())) {
                ok = false;
                break;
            }
        }
        encode_and_write(nullptr);  // flush encoder
    }

    av_write_trailer(out_fmt.get());
    av_channel_layout_uninit(&in_layout);

    if (ok) {
        CFW_LOG_DEBUG("[AudioParser] Exported audio -> '{}' ({})", path.string(), enc_codec->name);
    }
    return ok;
}

#else   // !CORONA_RESOURCE_HAVE_FFMPEG

std::shared_ptr<IResource> AudioParser::parse_audio_ffmpeg(const std::filesystem::path&) { return nullptr; }
bool AudioParser::export_audio(const IResource&, const std::filesystem::path&) { return false; }

#endif  // CORONA_RESOURCE_HAVE_FFMPEG

}  // namespace Corona::Resource
