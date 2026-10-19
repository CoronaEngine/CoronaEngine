#include <corona/systems/geometry/native_render_load_guard.h>
#include <stdexcept>

int main() {
    using Corona::Geometry::NativeRenderLoadGuard;
    if (!NativeRenderLoadGuard::is_loading(true, false)) {
        throw std::runtime_error("pending import must enter loading mode");
    }
    if (!NativeRenderLoadGuard::is_loading(false, true)) {
        throw std::runtime_error("pending build must enter loading mode");
    }
    if (NativeRenderLoadGuard::is_loading(false, false)) {
        throw std::runtime_error("ready scene must leave loading mode");
    }
    if (NativeRenderLoadGuard::effective_ssao(true, true, true)) {
        throw std::runtime_error("loading mode must disable ssao");
    }
    if (!NativeRenderLoadGuard::effective_ssao(false, true, true)) {
        throw std::runtime_error("ready scene must preserve ssao");
    }
}
