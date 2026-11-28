#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "corona/resource/resource.h"

namespace Corona::Resource {

/**
 * @brief 音频元数据
 */
struct AudioMetadata {
    int sample_rate = 0;            ///< 采样率（Hz）
    int channels = 0;               ///< 声道数
    std::int64_t frame_count = 0;   ///< 每声道样本数
    double duration_seconds = 0.0;  ///< 时长（秒）
    std::string codec_name;         ///< 源编码名（如 "aac"）
};

/**
 * @brief 音频资源
 *
 * 导入时将整段音频解码并重采样为交错 float32 PCM 存于内存。
 */
class Audio : public IResource {
   public:
    explicit Audio(const std::filesystem::path& path);
    ~Audio() override = default;

    /**
     * @brief 获取音频元数据
     */
    [[nodiscard]] const AudioMetadata& metadata() const { return meta_; }

    /**
     * @brief 交错 float32 PCM 样本
     *
     * 长度为 frame_count * channels，按声道交错存放。
     */
    [[nodiscard]] const std::vector<float>& samples() const { return pcm_; }

    /**
     * @brief 设置 PCM 数据
     *
     * @param samples 交错 float32 样本
     * @param sample_rate 采样率
     * @param channels 声道数
     */
    void set_samples(std::vector<float> samples, int sample_rate, int channels);

   private:
    AudioMetadata meta_;       ///< 元数据
    std::vector<float> pcm_;   ///< 交错 float32 PCM

    friend class AudioParser;
};

/**
 * @brief 音频解析器：导入解码为 PCM，导出编码到目标格式
 */
class AudioParser : public IParser {
   public:
    AudioParser();
    ~AudioParser() override = default;

   protected:
    /// 通过 miniaudio 解码 wav/mp3/flac/ogg
    std::shared_ptr<IResource> parse_audio(const std::filesystem::path& path);
    /// 通过 FFmpeg 解码 miniaudio 不支持的格式（aac/m4a 等）
    std::shared_ptr<IResource> parse_audio_ffmpeg(const std::filesystem::path& path);
    bool export_audio(const IResource& resource, const std::filesystem::path& path);
};

}  // namespace Corona::Resource
