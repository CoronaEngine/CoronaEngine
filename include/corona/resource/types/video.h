#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "corona/resource/resource.h"

namespace Corona::Resource {

/**
 * @brief 单帧视频解码结果（RGBA8）
 */
struct VideoFrame {
    std::vector<std::uint8_t> rgba;  ///< 像素数据，大小为 width*height*4
    int width = 0;                   ///< 帧宽
    int height = 0;                  ///< 帧高
    double pts_seconds = 0.0;        ///< 显示时间戳（秒）
};

/**
 * @brief 视频元数据（导入时立即读取，不需要解码全部帧）
 */
struct VideoMetadata {
    int width = 0;                  ///< 分辨率宽
    int height = 0;                 ///< 分辨率高
    double duration_seconds = 0.0;  ///< 时长（秒）
    double fps = 0.0;               ///< 平均帧率
    std::int64_t frame_count = 0;   ///< 帧数（容器提供，可能为估计值或 0）
    std::string codec_name;         ///< 视频编码名（如 "h264"）
    std::int64_t bit_rate = 0;      ///< 码率（bps）
};

/**
 * @brief 视频资源
 *
 * 导入时仅读取元数据并保留一个懒解码器，按需逐帧解码为 RGBA，
 * 避免将整段视频解码进内存。
 */
class Video : public IResource {
   public:
    explicit Video(const std::filesystem::path& path);
    ~Video() override;

    Video(const Video&) = delete;
    Video& operator=(const Video&) = delete;

    /**
     * @brief 获取视频元数据
     */
    [[nodiscard]] const VideoMetadata& metadata() const { return meta_; }

    /**
     * @brief 源文件路径（导出/转码时作为输入）
     */
    [[nodiscard]] const std::filesystem::path& source_path() const { return source_path_; }

    /**
     * @brief 顺序解码下一帧
     *
     * @return 成功返回 RGBA 帧；到达文件结尾或失败返回 std::nullopt
     */
    [[nodiscard]] std::optional<VideoFrame> decode_next_frame();

    /**
     * @brief 跳转到指定时间点
     *
     * 跳转后 decode_next_frame() 将从该时间点附近的关键帧开始。
     *
     * @param seconds 目标时间（秒）
     * @return 是否成功
     */
    bool seek(double seconds);

    /**
     * @brief 解码指定时间点最近的一帧（内部 seek + 解码）
     *
     * @param seconds 目标时间（秒）
     */
    [[nodiscard]] std::optional<VideoFrame> decode_frame_at(double seconds);

    /**
     * @brief 回到视频开头
     */
    void reset();

    /**
     * @brief 解码器是否成功打开
     */
    [[nodiscard]] bool is_open() const;

   private:
    friend class VideoParser;

    struct Decoder;                       ///< pImpl，隐藏 FFmpeg 句柄
    std::unique_ptr<Decoder> decoder_;    ///< 懒解码器
    VideoMetadata meta_;                  ///< 元数据
    std::filesystem::path source_path_;   ///< 源文件路径
};

/**
 * @brief 视频解析器：导入读取元数据/按需解码，导出做转码
 */
class VideoParser : public IParser {
   public:
    VideoParser();
    ~VideoParser() override = default;

   protected:
    std::shared_ptr<IResource> parse_video(const std::filesystem::path& path);
    bool export_video(const IResource& resource, const std::filesystem::path& path);
};

}  // namespace Corona::Resource
