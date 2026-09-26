#include <corona/events/acoustics_system_events.h>
#include <horizon/core/logging.h>
#include <corona/kernel/event/i_event_bus.h>
#include <corona/kernel/event/i_event_stream.h>
#include <corona/resource/resource_manager.h>
#include <corona/resource/types/audio.h>
#include <corona/shared_data_hub.h>
#include <corona/systems/acoustics/acoustics_system.h>

namespace Corona::Systems {

bool AcousticsSystem::initialize(Kernel::ISystemContext* ctx) {
    CFW_LOG_NOTICE("AcousticsSystem: Initializing...");

    // --- miniaudio engine 初始化（自带默认设备 + 混音） ---
    ma_result res = ma_engine_init(nullptr, &engine_);
    if (res != MA_SUCCESS) {
        CFW_LOG_ERROR("AcousticsSystem: ma_engine_init failed: {}", static_cast<int>(res));
        return false;
    }
    engine_ready_ = true;

    CFW_LOG_NOTICE("AcousticsSystem: miniaudio engine initialized successfully");

    // --- 订阅播放事件 ---
    if (auto* event_bus = ctx->event_bus()) {
        event_bus->subscribe<Events::PlayAudioEvent>(
            [this](const Events::PlayAudioEvent& ev) {
                std::lock_guard lock(cmd_mutex_);
                pending_plays_.push_back(ev);
            });
        event_bus->subscribe<Events::StopAudioEvent>(
            [this](const Events::StopAudioEvent& ev) {
                std::lock_guard lock(cmd_mutex_);
                pending_stops_.push_back(ev);
            });
        CFW_LOG_NOTICE("AcousticsSystem: subscribed to PlayAudioEvent / StopAudioEvent");
    } else {
        CFW_LOG_WARNING("AcousticsSystem: event_bus not available, audio playback commands disabled");
    }

    return true;
}

void AcousticsSystem::destroy_playback(ActivePlayback& ap) {
    // 顺序关键：先停 sound（解除对 buffer 的引用），再放 buffer，最后 handle 析构。
    if (ap.sound_ready) {
        ma_sound_uninit(&ap.sound);
        ap.sound_ready = false;
    }
    if (ap.buffer_ready) {
        ma_audio_buffer_uninit(&ap.buffer);
        ap.buffer_ready = false;
    }
    // ap.handle 在 ActivePlayback 析构时自动释放（unref + 解锁）。
}

void AcousticsSystem::process_play_request(std::uint64_t resource_id, bool loop, std::uintptr_t acoustics_handle) {
    if (!engine_ready_) {
        return;
    }

    const bool spatial = (acoustics_handle != 0);

    // 如果已在播放，先停掉旧实例（空间音频按句柄去重，全局按 resource_id 去重）
    process_stop_request(resource_id, acoustics_handle);

    // 从资源管理器取 Audio（持有 handle 保活 + 防淘汰，整段播放期间有效）
    auto handle = Resource::ResourceManager::get_instance().acquire_read<Resource::Audio>(resource_id);
    if (!handle) {
        CFW_LOG_ERROR("[AcousticsSystem] play: resource {} not found or not Audio/ready", resource_id);
        return;
    }

    const auto& meta = handle->metadata();
    const auto& pcm = handle->samples();
    if (pcm.empty() || meta.channels <= 0) {
        CFW_LOG_WARNING("[AcousticsSystem] play: resource {} has no/invalid PCM data", resource_id);
        return;
    }

    auto ap = std::make_unique<ActivePlayback>();
    ap->resource_id = resource_id;
    ap->acoustics_handle = acoustics_handle;
    ap->spatial = spatial;
    ap->loop = loop;
    ap->handle = std::move(handle);

    const auto& ap_pcm = ap->handle->samples();
    const ma_uint64 frame_count = static_cast<ma_uint64>(ap_pcm.size() / meta.channels);

    // ma_audio_buffer 直接引用资源 PCM（零拷贝）。
    ma_audio_buffer_config cfg = ma_audio_buffer_config_init(
        ma_format_f32,
        static_cast<ma_uint32>(meta.channels),
        frame_count,
        ap_pcm.data(),
        nullptr);

    ma_result res = ma_audio_buffer_init(&cfg, &ap->buffer);
    if (res != MA_SUCCESS) {
        CFW_LOG_ERROR("[AcousticsSystem] play: ma_audio_buffer_init failed: {}", static_cast<int>(res));
        return;
    }
    ap->buffer_ready = true;

    res = ma_sound_init_from_data_source(&engine_, &ap->buffer, 0, nullptr, &ap->sound);
    if (res != MA_SUCCESS) {
        CFW_LOG_ERROR("[AcousticsSystem] play: ma_sound_init_from_data_source failed: {}", static_cast<int>(res));
        destroy_playback(*ap);
        return;
    }
    ap->sound_ready = true;

    if (spatial) {
        // 空间音频：开启空间化 + 反比衰减。注意 miniaudio 只对单声道做 3D 声像，
        // 立体声源会播放但不随位置 pan（已知限制）。
        ma_sound_set_spatialization_enabled(&ap->sound, MA_TRUE);
        ma_sound_set_attenuation_model(&ap->sound, ma_attenuation_model_inverse);
        if (meta.channels > 1) {
            CFW_LOG_WARNING("[AcousticsSystem] play: rid={} is {}ch; miniaudio only spatializes mono — sound will not pan",
                            resource_id, meta.channels);
        }
    } else {
        // 全局非空间播放。
        ma_sound_set_spatialization_enabled(&ap->sound, MA_FALSE);
    }

    ma_sound_set_looping(&ap->sound, loop ? MA_TRUE : MA_FALSE);

    res = ma_sound_start(&ap->sound);
    if (res != MA_SUCCESS) {
        CFW_LOG_ERROR("[AcousticsSystem] play: ma_sound_start failed: {}", static_cast<int>(res));
        destroy_playback(*ap);
        return;
    }

    std::lock_guard lock(playback_mutex_);
    active_playbacks_.push_back(std::move(ap));

    CFW_LOG_INFO("[AcousticsSystem] play started: rid={} {}Hz {}ch loop={} spatial={}",
                 resource_id, meta.sample_rate, meta.channels, loop, spatial);
}

void AcousticsSystem::process_stop_request(std::uint64_t resource_id, std::uintptr_t acoustics_handle) {
    std::lock_guard lock(playback_mutex_);
    for (auto it = active_playbacks_.begin(); it != active_playbacks_.end();) {
        if (!*it) {
            it = active_playbacks_.erase(it);
            continue;
        }
        const auto& ap = **it;
        // 空间音频按句柄匹配；全局按 resource_id 匹配。
        const bool match = (acoustics_handle != 0)
                               ? (ap.acoustics_handle == acoustics_handle)
                               : (!ap.spatial && ap.resource_id == resource_id);
        if (match) {
            destroy_playback(**it);
            it = active_playbacks_.erase(it);
        } else {
            ++it;
        }
    }
}

void AcousticsSystem::update() {
    // --- 消费命令队列 ---
    {
        std::lock_guard lock(cmd_mutex_);
        for (const auto& cmd : pending_plays_) {
            process_play_request(cmd.resource_id, cmd.loop, cmd.acoustics_handle);
        }
        pending_plays_.clear();
        for (const auto& cmd : pending_stops_) {
            process_stop_request(cmd.resource_id, cmd.acoustics_handle);
        }
        pending_stops_.clear();
    }

    // --- 空间音频：更新 listener（活动相机）与各空间声音位置 ---
    update_spatial();

    // --- 监控各播放：loop 由 miniaudio 自动循环；非 loop 播完则清理 ---
    {
        std::lock_guard lock(playback_mutex_);

        auto& acoustics_storage = SharedDataHub::instance().acoustics_storage();

        auto it = active_playbacks_.begin();
        while (it != active_playbacks_.end()) {
            auto& ap = **it;
            if (!ap.sound_ready) {
                ++it;
                continue;
            }

            // 空间音频：其声学组件被释放（actor 删除）→ 当作结束清理，避免悬空。
            bool finished = false;
            if (ap.spatial) {
                auto acc = acoustics_storage.try_acquire_read(ap.acoustics_handle);
                if (!acc.valid()) {
                    finished = true;
                }
            }
            // 非循环播完。
            if (!finished && !ap.loop && ma_sound_at_end(&ap.sound) == MA_TRUE) {
                finished = true;
            }

            if (finished) {
                CFW_LOG_INFO("[AcousticsSystem] playback finished: rid={} spatial={}", ap.resource_id, ap.spatial);
                destroy_playback(ap);
                it = active_playbacks_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void AcousticsSystem::update_spatial() {
    auto& hub = SharedDataHub::instance();

    // --- (a) listener ← 场景活动相机 ---
    {
        auto& scene_storage = hub.scene_storage();
        auto& camera_storage = hub.camera_storage();
        for (const auto& scene : scene_storage) {
            if (!scene.enabled || scene.active_camera_handle == 0) {
                continue;
            }
            auto cam = camera_storage.try_acquire_read(scene.active_camera_handle);
            if (cam.valid()) {
                ma_engine_listener_set_position(&engine_, 0, cam->position.x, cam->position.y, cam->position.z);
                ma_engine_listener_set_direction(&engine_, 0, cam->forward.x, cam->forward.y, cam->forward.z);
                ma_engine_listener_set_world_up(&engine_, 0, cam->world_up.x, cam->world_up.y, cam->world_up.z);
            }
            break;  // 用第一个有活动相机的场景
        }
    }

    // --- (b) 每个空间声音的位置 ← actor transform ---
    {
        std::lock_guard lock(playback_mutex_);
        if (active_playbacks_.empty()) {
            return;
        }

        auto& acoustics_storage = hub.acoustics_storage();
        auto& geometry_storage = hub.geometry_storage();
        auto& transform_storage = hub.model_transform_storage();

        for (auto& ap_ptr : active_playbacks_) {
            auto& ap = *ap_ptr;
            if (!ap.spatial || !ap.sound_ready) {
                continue;
            }

            std::uintptr_t geometry_handle = 0;
            auto acc = acoustics_storage.try_acquire_read(ap.acoustics_handle);
            if (!acc.valid()) {
                continue;  // 组件已释放，留给监控循环清理
            }
            geometry_handle = acc->geometry_handle;
            ma_sound_set_volume(&ap.sound, acc->volume);
            ma_sound_set_spatialization_enabled(&ap.sound, acc->audio_enabled ? MA_TRUE : MA_FALSE);

            if (geometry_handle == 0) {
                continue;
            }
            auto geo = geometry_storage.try_acquire_read(geometry_handle);
            if (!geo.valid() || geo->transform_handle == 0) {
                continue;
            }
            auto xf = transform_storage.try_acquire_read(geo->transform_handle);
            if (xf.valid()) {
                ma_sound_set_position(&ap.sound, xf->position.x, xf->position.y, xf->position.z);
            }
        }
    }
}

void AcousticsSystem::shutdown() {
    CFW_LOG_NOTICE("AcousticsSystem: Shutting down...");

    {
        std::lock_guard lock(playback_mutex_);
        for (auto& ap : active_playbacks_) {
            if (ap) {
                destroy_playback(*ap);
            }
        }
        active_playbacks_.clear();
    }

    if (engine_ready_) {
        ma_engine_uninit(&engine_);
        engine_ready_ = false;
    }

    CFW_LOG_NOTICE("AcousticsSystem: Shutdown complete");
}

}  // namespace Corona::Systems
