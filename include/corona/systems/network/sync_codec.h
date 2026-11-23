#pragma once

/// 网络编辑状态的**线上编码**（纯值，无 registry 访问）。
///
/// 这些结构是协议的一部分：字段顺序、字段类型与 `sizeof(OpticsPacked)` 决定线上字节，
/// 不得为了"看起来更整齐"重排。搬到这里只是为了让它同时服务于两个线程：
///
/// - 拥有者线程（DAG tick 边界）从组件编码出 `value` 字节；
/// - 网络线程从 `value` 字节解码出待发送/待落地的值（编码/解码本身不碰 registry）。
///
/// 哈希函数只用于**脏值判定**（`last_synced` 只在内存里），不上线。迁移前的实现把
/// `ktm::fvec3` 的整个 16 字节存储（含 padding）混进哈希；这里改为混"线上值的 9 个
/// float"，因此哈希值与旧实现不同 —— 但两端（拥有者线程的出站帧与网络线程的入站记账）
/// 用的是同一个函数，且没有持久化，所以判定语义等价：**线上字节逐字节相同，哈希输入
/// 变干净了**。

#include <corona/systems/network/protocol.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace Corona::Network {

/// 远程 optics 包承载整份材质快照（线上布局 = 协议）。
///
/// `bool` 必须是 1 字节，否则长度会变、对端会按畸形载荷丢弃。用 `static_assert`
/// 把长度钉死（迁移前是 `sizeof(OpticsPacked)` = 84）。
struct OpticsPacked {
    bool visible{true};
    bool bEnableLighting{true};
    float metallic{0.0f};
    float roughness{0.5f};
    float subsurface{0.0f};
    float specular{0.5f};
    float specularTint{0.0f};
    float anisotropic{0.0f};
    float sheen{0.0f};
    float sheenTint{0.5f};
    float clearcoat{0.0f};
    float clearcoatGloss{1.0f};
    float ambient[3]{0.2f, 0.2f, 0.2f};
    float diffuse[3]{0.8f, 0.8f, 0.8f};
    float specular_color[3]{1.0f, 1.0f, 1.0f};
    float shininess{32.0f};
};

static_assert(sizeof(OpticsPacked) == 84,
              "OpticsPacked is the wire layout: its size must stay 84 bytes");

/// ModelTransform 的线上值：position(3) + euler_rotation(3) + scale(3)。
struct TransformState {
    float xform[9]{};
};

/// Environment 的线上值：sun_position(3) + sun_intensity + exposure。
struct EnvironmentState {
    float sun_position[3]{0.0f, 1.0f, 0.0f};
    float sun_intensity{10.0f};
    float exposure{1.0f};
};

/// 身份解析的一行：出站帧需要 `(storage, seq) → (actor guid, 本地所有权)`。
///
/// 由网络线程每帧从身份注册表取一次快照（一次锁、O(actors)），再随帧请求交给拥有者
/// 线程做 O(1) 查表。这样 tick 边界上不再对每个实体做一次 O(actors) 线性扫描。
struct SyncIdentityEntry {
    StorageID storage{StorageID::ST_MODEL_TRANSFORM};
    std::uint64_t entity_seq{0};
    std::string actor_guid;
    bool locally_owned{true};
};

namespace sync_codec_detail {

inline void mix_bytes(std::uint64_t& hash, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash = hash * 31 + bytes[i];
    }
}

}  // namespace sync_codec_detail

// ----------------------------------------------------------------------------
// 脏值哈希（输入是线上值，不含 fvec3/结构体 padding）
// ----------------------------------------------------------------------------

[[nodiscard]] inline std::uint64_t hash_transform(const TransformState& state,
                                                  std::uint64_t entity_seq) {
    std::uint64_t hash = entity_seq;
    sync_codec_detail::mix_bytes(hash, state.xform, sizeof(state.xform));
    return hash;
}

[[nodiscard]] inline std::uint64_t hash_optics(const OpticsPacked& packed,
                                               std::uint64_t entity_seq) {
    std::uint64_t hash = entity_seq;
    sync_codec_detail::mix_bytes(hash, &packed.visible, sizeof(packed.visible));
    sync_codec_detail::mix_bytes(hash, &packed.bEnableLighting, sizeof(packed.bEnableLighting));
    sync_codec_detail::mix_bytes(hash, &packed.metallic, sizeof(packed.metallic));
    sync_codec_detail::mix_bytes(hash, &packed.roughness, sizeof(packed.roughness));
    sync_codec_detail::mix_bytes(hash, &packed.subsurface, sizeof(packed.subsurface));
    sync_codec_detail::mix_bytes(hash, &packed.specular, sizeof(packed.specular));
    sync_codec_detail::mix_bytes(hash, &packed.specularTint, sizeof(packed.specularTint));
    sync_codec_detail::mix_bytes(hash, &packed.anisotropic, sizeof(packed.anisotropic));
    sync_codec_detail::mix_bytes(hash, &packed.sheen, sizeof(packed.sheen));
    sync_codec_detail::mix_bytes(hash, &packed.sheenTint, sizeof(packed.sheenTint));
    sync_codec_detail::mix_bytes(hash, &packed.clearcoat, sizeof(packed.clearcoat));
    sync_codec_detail::mix_bytes(hash, &packed.clearcoatGloss, sizeof(packed.clearcoatGloss));
    sync_codec_detail::mix_bytes(hash, packed.ambient, sizeof(packed.ambient));
    sync_codec_detail::mix_bytes(hash, packed.diffuse, sizeof(packed.diffuse));
    sync_codec_detail::mix_bytes(hash, packed.specular_color, sizeof(packed.specular_color));
    sync_codec_detail::mix_bytes(hash, &packed.shininess, sizeof(packed.shininess));
    return hash;
}

[[nodiscard]] inline std::uint64_t hash_environment(const EnvironmentState& state,
                                                    std::uint64_t entity_seq) {
    std::uint64_t hash = entity_seq;
    sync_codec_detail::mix_bytes(hash, state.sun_position, sizeof(state.sun_position));
    sync_codec_detail::mix_bytes(hash, &state.sun_intensity, sizeof(state.sun_intensity));
    sync_codec_detail::mix_bytes(hash, &state.exposure, sizeof(state.exposure));
    return hash;
}

// ----------------------------------------------------------------------------
// 编码（拥有者线程从组件 → 线上字节）
// ----------------------------------------------------------------------------

[[nodiscard]] inline std::vector<std::uint8_t> encode_transform(const TransformState& state) {
    std::vector<std::uint8_t> value(sizeof(state.xform));
    std::memcpy(value.data(), state.xform, sizeof(state.xform));
    return value;
}

[[nodiscard]] inline std::vector<std::uint8_t> encode_optics(const OpticsPacked& packed) {
    // 整份快照逐字节拷贝（含编译器插入的 padding），与迁移前一致：对端用同一个
    // `sizeof(OpticsPacked)` 校验长度，不解释 padding。
    std::vector<std::uint8_t> value(sizeof(packed));
    std::memcpy(value.data(), &packed, sizeof(packed));
    return value;
}

[[nodiscard]] inline std::vector<std::uint8_t> encode_environment(
    const EnvironmentState& state) {
    std::vector<std::uint8_t> value(5 * sizeof(float));
    std::memcpy(value.data(), state.sun_position, sizeof(state.sun_position));
    std::memcpy(value.data() + sizeof(state.sun_position), &state.sun_intensity,
                sizeof(state.sun_intensity));
    std::memcpy(value.data() + sizeof(state.sun_position) + sizeof(state.sun_intensity),
                &state.exposure, sizeof(state.exposure));
    return value;
}

// ----------------------------------------------------------------------------
// 解码（网络线程从线上字节 → 值；长度必须**精确**匹配，畸形载荷一律失败）
// ----------------------------------------------------------------------------

[[nodiscard]] inline bool decode_transform(const std::uint8_t* value,
                                           std::size_t value_len,
                                           TransformState& out) {
    if (value == nullptr || value_len != sizeof(out.xform)) {
        return false;
    }
    std::memcpy(out.xform, value, sizeof(out.xform));
    return true;
}

[[nodiscard]] inline bool decode_optics(const std::uint8_t* value, std::size_t value_len,
                                        OpticsPacked& out) {
    if (value == nullptr || value_len != sizeof(out)) {
        return false;
    }
    std::memcpy(&out, value, sizeof(out));
    return true;
}

[[nodiscard]] inline bool decode_environment(const std::uint8_t* value,
                                             std::size_t value_len,
                                             EnvironmentState& out) {
    if (value == nullptr || value_len != 5 * sizeof(float)) {
        return false;
    }
    std::memcpy(out.sun_position, value, sizeof(out.sun_position));
    std::memcpy(&out.sun_intensity, value + sizeof(out.sun_position),
                sizeof(out.sun_intensity));
    std::memcpy(&out.exposure,
                value + sizeof(out.sun_position) + sizeof(out.sun_intensity),
                sizeof(out.exposure));
    return true;
}

}  // namespace Corona::Network
