///
/// Created by Zero on 2023/7/17.
/// Updated on 2026/4/30 to define the public render-service contract.
///

#pragma once

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
#define VISION_SDK_EXTERN_C extern "C"
#define VISION_SDK_NOEXCEPT noexcept
#else
#define VISION_SDK_EXTERN_C
#define VISION_SDK_NOEXCEPT
#endif

#if defined(_MSC_VER)
#if defined(VISION_SDK_EXPORT_DLL)
#define VISION_SDK_API VISION_SDK_EXTERN_C __declspec(dllexport)
#else
#define VISION_SDK_API VISION_SDK_EXTERN_C __declspec(dllimport)
#endif
#else
#define VISION_SDK_API VISION_SDK_EXTERN_C
#endif

#define VISION_SDK_ABI_VERSION 0x00020000u

/// Stable ABI surface for CoronaEngine <-> Vision integration.
///
/// Design rules:
/// 1. The engine must not include Vision internal headers such as Pipeline, FrameBuffer, or NodeDesc.
/// 2. Scene exchange must happen through versioned packets, not internal C++ objects.
/// 3. Result transport must support CPU memory and external GPU handles.
/// 4. Long-lived rendering state belongs to a session, not to one-off render calls.

typedef struct VisionServiceHandle_t *VisionServiceHandle;
typedef struct VisionSessionHandle_t *VisionSessionHandle;

enum VisionStatusCode : uint32_t {
	VisionStatusCode_Success = 0,
	VisionStatusCode_InvalidArgument = 1,
	VisionStatusCode_InvalidState = 2,
	VisionStatusCode_Unsupported = 3,
	VisionStatusCode_DeviceError = 4,
	VisionStatusCode_SceneError = 5,
	VisionStatusCode_InternalError = 6,
};

enum VisionScenePacketFormat : uint32_t {
	/// Direct pass-through of Vision's existing project JSON.
	VisionScenePacketFormat_ProjectJsonUtf8 = 1,
	/// Binary snapshot emitted by CoronaEngine's bridge layer.
	VisionScenePacketFormat_CoronaEngineSnapshotV1 = 2,
};

enum VisionAOV : uint32_t {
	VisionAOV_FinalColor = 0,
	VisionAOV_Albedo = 1,
	VisionAOV_Normal = 2,
	VisionAOV_Depth = 3,
	VisionAOV_ObjectId = 4,
	VisionAOV_Visibility = 5,
};

enum VisionImageFormat : uint32_t {
	VisionImageFormat_Unknown = 0,
	VisionImageFormat_RGBA32Float = 1,
	VisionImageFormat_RGBA16Float = 2,
	VisionImageFormat_RGBA8UNorm = 3,
	VisionImageFormat_R32UInt = 4,
};

enum VisionOutputTargetType : uint32_t {
	VisionOutputTargetType_None = 0,
	VisionOutputTargetType_CpuImage = 1,
	VisionOutputTargetType_ExternalTexture = 2,
	VisionOutputTargetType_ExternalBuffer = 3,
	VisionOutputTargetType_File = 4,
};

enum VisionSessionFlags : uint32_t {
	VisionSessionFlag_None = 0,
	VisionSessionFlag_EnableValidation = 1u << 0u,
	VisionSessionFlag_EnableShaderCache = 1u << 1u,
	VisionSessionFlag_KeepAccumulation = 1u << 2u,
};

enum VisionRenderFlags : uint32_t {
	VisionRenderFlag_None = 0,
	VisionRenderFlag_ResetAccumulation = 1u << 0u,
	VisionRenderFlag_BlockUntilFinished = 1u << 1u,
	VisionRenderFlag_ReturnStats = 1u << 2u,
};

struct VisionStringView {
	const char *data{};
	size_t size{};
};

struct VisionExtent2D {
	uint32_t width{};
	uint32_t height{};
};

struct VisionExternalMemory {
	uint64_t os_handle{};
	uint64_t byte_size{};
	uint32_t reserved{};
};

struct VisionScenePacket {
	VisionScenePacketFormat format{VisionScenePacketFormat_ProjectJsonUtf8};
	uint32_t schema_version{1u};
	uint32_t flags{};
	const void *data{};
	uint64_t size_bytes{};
};

struct VisionOutputTarget {
	VisionOutputTargetType type{VisionOutputTargetType_None};
	VisionImageFormat format{VisionImageFormat_Unknown};
	VisionExtent2D extent{};
	void *cpu_ptr{};
	uint64_t cpu_size_bytes{};
	uint64_t row_pitch_bytes{};
	VisionExternalMemory external_memory{};
	VisionStringView file_path{};
};

struct VisionServiceCreateInfo {
	uint32_t reserved{};
	VisionStringView runtime_dir{};
	VisionStringView plugin_dir{};
	VisionStringView shader_cache_dir{};
};

struct VisionSessionCreateInfo {
	VisionExtent2D initial_extent{};
	uint32_t flags{VisionSessionFlag_EnableShaderCache};
	uint32_t max_in_flight_requests{1u};
	VisionStringView debug_name{};
};

struct VisionRenderRequest {
	uint64_t request_id{};
	VisionScenePacket scene{};
	VisionOutputTarget output{};
	VisionStringView camera_name{};
	VisionAOV aov{VisionAOV_FinalColor};
	uint32_t spp{};
	uint32_t max_depth{};
	uint32_t flags{VisionRenderFlag_BlockUntilFinished | VisionRenderFlag_ReturnStats};
	uint32_t time_budget_ms{};
};

struct VisionRenderStats {
	double scene_update_ms{};
	double prepare_ms{};
	double render_ms{};
	double download_ms{};
	uint32_t rendered_spp{};
	uint32_t frame_index{};
};

struct VisionRenderResult {
	VisionStatusCode status{VisionStatusCode_InternalError};
	VisionImageFormat format{VisionImageFormat_Unknown};
	VisionExtent2D extent{};
	uint64_t produced_bytes{};
	VisionRenderStats stats{};
};

/// Returns the stable ABI version encoded as major << 16 | minor.
VISION_SDK_API uint32_t vision_sdk_abi_version(void) VISION_SDK_NOEXCEPT;

/// Returns a short human-readable identifier for the SDK binary.
VISION_SDK_API const char *vision_sdk_name(void) VISION_SDK_NOEXCEPT;