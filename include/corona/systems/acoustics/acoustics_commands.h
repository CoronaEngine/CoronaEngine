#pragma once

/// Acoustics 的跨线程交互边界（P3：写走命令，读走快照）。
///
/// 只有三个可写标量：volume / audio_enabled / resource_id。
/// `play()` / `stop()` 本来就是命令（事件总线），不在这一层。

#include <corona/kernel/command/command_buffer.h>
#include <corona/kernel/command/snapshot_channel.h>
#include <corona/shared_data_hub.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace Corona {

enum class AcousticsField : std::uint32_t {
    None = 0,
    Volume = 1u << 0,
    AudioEnabled = 1u << 1,
    ResourceId = 1u << 2,
};

constexpr AcousticsField operator|(AcousticsField lhs, AcousticsField rhs) {
    return static_cast<AcousticsField>(static_cast<std::uint32_t>(lhs) |
                                       static_cast<std::uint32_t>(rhs));
}

constexpr bool has_acoustics_field(AcousticsField fields, AcousticsField field) {
    return (static_cast<std::uint32_t>(fields) & static_cast<std::uint32_t>(field)) != 0;
}

struct AcousticsUpdateCommand {
    std::uintptr_t acoustics_handle{};
    AcousticsField fields{AcousticsField::None};
    float volume{1.0f};
    bool audio_enabled{true};
    std::uint64_t resource_id{0};
};

struct AcousticsUpdateKey {
    static std::uintptr_t get(const AcousticsUpdateCommand& command) noexcept {
        return command.acoustics_handle;
    }
};

struct AcousticsSnapshotRow {
    std::uintptr_t handle{0};
    float volume{1.0f};
    bool audio_enabled{true};
    std::uint64_t resource_id{0};
};

struct AcousticsSnapshot {
    std::uint64_t tick{0};
    std::vector<AcousticsSnapshotRow> rows;
};

class AcousticsCommandChannel {
   public:
    using Buffer = Kernel::CommandBuffer<AcousticsUpdateCommand, AcousticsUpdateKey,
                                         Kernel::CoalescePolicy::LatestWins>;

    static AcousticsCommandChannel& instance() {
        static AcousticsCommandChannel channel;
        return channel;
    }

    [[nodiscard]] Buffer& commands() { return commands_; }
    [[nodiscard]] Kernel::SnapshotChannel<AcousticsSnapshot>& snapshots() { return snapshots_; }

   private:
    Buffer commands_;
    Kernel::SnapshotChannel<AcousticsSnapshot> snapshots_;
};

inline AcousticsSnapshotRow make_acoustics_row(std::uintptr_t handle, const AcousticsDevice& device) {
    AcousticsSnapshotRow row;
    row.handle = handle;
    row.volume = device.volume;
    row.audio_enabled = device.audio_enabled;
    row.resource_id = device.resource_id;
    return row;
}

/// 拥有者线程：从槽位重建整份快照
inline void publish_acoustics_snapshot_from_storage() {
    auto& storage = SharedDataHub::instance().acoustics_storage();
    AcousticsSnapshot snapshot;
    snapshot.tick = AcousticsCommandChannel::instance().snapshots().revision() + 1;
    for (const auto& device : std::as_const(storage)) {
        const auto handle = reinterpret_cast<std::uintptr_t>(static_cast<const void*>(&device));
        if (handle == 0) {
            continue;
        }
        snapshot.rows.push_back(make_acoustics_row(handle, device));
    }
    AcousticsCommandChannel::instance().snapshots().publish(std::move(snapshot));
}

[[nodiscard]] inline std::optional<AcousticsSnapshotRow> acoustics_row(std::uintptr_t handle) {
    if (handle == 0) {
        return std::nullopt;
    }
    auto snapshot = AcousticsCommandChannel::instance().snapshots().read();
    if (!snapshot) {
        return std::nullopt;
    }
    for (const auto& row : snapshot->rows) {
        if (row.handle == handle) {
            return row;
        }
    }
    return std::nullopt;
}

/// 有效值 = 快照（已提交）+ 待处理命令（pending）。
/// 没有快照但有 pending 时也要能读出来 —— 例如"刚绑定资源就 play()"。
[[nodiscard]] inline std::optional<AcousticsSnapshotRow> acoustics_view(std::uintptr_t handle) {
    if (handle == 0) {
        return std::nullopt;
    }
    std::optional<AcousticsSnapshotRow> view = acoustics_row(handle);
    if (const auto pending = AcousticsCommandChannel::instance().commands().peek(handle)) {
        if (!view.has_value()) {
            view.emplace();
            view->handle = handle;
        }
        if (has_acoustics_field(pending->fields, AcousticsField::Volume)) {
            view->volume = pending->volume;
        }
        if (has_acoustics_field(pending->fields, AcousticsField::AudioEnabled)) {
            view->audio_enabled = pending->audio_enabled;
        }
        if (has_acoustics_field(pending->fields, AcousticsField::ResourceId)) {
            view->resource_id = pending->resource_id;
        }
    }
    return view;
}

}  // namespace Corona
