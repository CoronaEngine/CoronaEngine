#pragma once
#include <cstdint>

#if defined(_WIN32)
#if defined(CORONA_HOTFIX_EXPORTS)
#define CORONA_HOTFIX_API __declspec(dllexport)
#else
#define CORONA_HOTFIX_API __declspec(dllimport)
#endif
#else
#define CORONA_HOTFIX_API __attribute__((visibility("default")))
#endif

namespace Corona::Hotfix {
// Initialization, callback delivery and shutdown belong to the renderer thread.
[[nodiscard]] CORONA_HOTFIX_API bool initialize();
CORONA_HOTFIX_API void pump();
// Advances once per nonempty applied batch, including previous/next versions.
// The counter survives shutdown so renderers can compare revisions safely.
[[nodiscard]] CORONA_HOTFIX_API std::uint64_t revision() noexcept;
// Call after releasing runtime objects, while their device remains available.
// Horizon retains loaded patch code for the lifetime of the process.
CORONA_HOTFIX_API void shutdown();
// Generated once per normal module, never compiled into a staged patch.
CORONA_HOTFIX_API void register_target(const char8_t* source, const char* target);
}
