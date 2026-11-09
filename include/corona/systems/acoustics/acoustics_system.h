#pragma once

#include <corona/kernel/entity_id.h>
#include <corona/events/acoustics_system_events.h>
#include <corona/kernel/event/i_event_bus.h>
#include <corona/kernel/event/i_event_stream.h>
#include <corona/kernel/system/system_base.h>
#include <corona/resource/resource_cache.h>
#include <corona/resource/types/audio.h>

#include <miniaudio.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace Corona::Systems {

/**
 * @brief 声学系统 (Acoustics System)
 *
 * 负责管理声音播放、混音和音频处理。
 * 运行在独立线程，以 60 FPS 处理声学逻辑。
 *
 * 当前实现：基于 miniaudio 的全局播放器（非空间音效）。
 * 使用单个 ma_engine 自动混音，支持同时播放多个音频。
 * 每个活跃音频拥有一个 ma_sound，由 ma_audio_buffer 直接引用资源 PCM
 * （零拷贝）；通过持有 ResourceManager 的 ReadHandle 保活资源，避免播放
 * 期间被淘汰/释放。
 */
class AcousticsSystem : public Kernel::SystemBase {
   public:
    AcousticsSystem() { set_target_fps(60); }
    ~AcousticsSystem() override = default;

    std::string_view get_name() const override { return "Acoustics"; }
    int get_priority() const override { return 70; }

    bool initialize(Kernel::ISystemContext* ctx) override;
    void update() override;
    void shutdown() override;

   private:
    /// 单个活跃播放状态
    ///
    /// 注意：ma_sound 内部引用 buffer 的地址，故该结构地址必须稳定 ——
    /// 用 unique_ptr 持有并存入 vector，保证元素不随容器扩容而搬移。
    struct ActivePlayback {
        std::uint64_t resource_id{0};
        EntityId acoustics_handle{0};  // 空间音频主键（AcousticsStorage handle）；0=全局非空间
        bool spatial{false};                 // acoustics_handle != 0
        Resource::ReadHandle<Resource::Audio> handle;  // 保活 + 防淘汰，PCM 零拷贝来源
        ma_audio_buffer buffer{};                      // 指向 handle->samples()
        ma_sound sound{};                              // 由 buffer 供数据
        bool loop{false};
        bool sound_ready{false};   // sound 是否已 init（需要 uninit）
        bool buffer_ready{false};  // buffer 是否已 init（需要 uninit）
    };

    /// 处理播放命令（在 update 线程中调用）
    void process_play_request(std::uint64_t resource_id, bool loop, EntityId acoustics_handle);
    /// 处理停止命令：acoustics_handle != 0 时按句柄停止，否则按 resource_id 停止
    void process_stop_request(std::uint64_t resource_id, EntityId acoustics_handle);
    /// 释放单个播放占用的 miniaudio 资源（不从容器移除）
    static void destroy_playback(ActivePlayback& ap);
    /// 每帧更新 listener（活动相机）与各空间声音的位置
    void update_spatial();

    // miniaudio 音频后端
    ma_engine engine_{};
    bool engine_ready_{false};

    // 播放状态（unique_ptr 保证 ma_sound/ma_audio_buffer 地址稳定）
    std::mutex playback_mutex_;
    std::vector<std::unique_ptr<ActivePlayback>> active_playbacks_;

    // 命令队列（由 API/主线程写入，update 线程消费）
    std::mutex cmd_mutex_;
    std::vector<Events::PlayAudioEvent> pending_plays_;
    std::vector<Events::StopAudioEvent> pending_stops_;
};

}  // namespace Corona::Systems
