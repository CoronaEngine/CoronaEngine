"""Platform adoption must let the UI thread return before Display consumes its queue."""
from pathlib import Path
import shutil
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[3]


def test_platform_adoption_does_not_wait_for_display(tmp_path):
    compiler = shutil.which('g++') or shutil.which('clang++')
    if not compiler:
        pytest.skip('Standalone window registration regression requires a C++ compiler')
    source = (ROOT / 'src/systems/ui/sdl/sdl_window_manager.cpp').read_text(encoding='utf-8')
    start = source.index('bool SdlWindowManager::adopt_platform_window(')
    end = source.index('\nvoid SdlWindowManager::forget_platform_window', start)
    sdl = tmp_path / 'SDL3'
    sdl.mkdir()
    (sdl / 'SDL.h').write_text('''#pragma once
#include <cstdint>
using SDL_WindowID = std::uint32_t;
struct SDL_Window { SDL_WindowID id; void* surface; };
inline SDL_WindowID SDL_GetWindowID(SDL_Window* window) { return window->id; }
''', encoding='utf-8')
    harness = tmp_path / 'registration.cpp'
    harness.write_text('''
#include <corona/systems/ui/sdl_window_manager.h>
#include <corona/systems/display/display_surface_commands.h>
#include <iostream>
namespace Corona::Kernel {
struct KernelContext {
    static KernelContext& instance() { static KernelContext context; return context; }
    void* event_bus() { return this; }
};
}
namespace Corona::Events {
struct DisplaySurfaceChangedEvent {
    void* surface = nullptr;
    std::optional<Systems::UI::SurfaceCompletionTicket> registration_ticket;
};
}
namespace Corona::Systems::UI {
void* native_surface_from_sdl_window(SDL_Window* window) {
    return window ? window->surface : nullptr;
}
''' + source[start:end] + '''
}
int main() {
    using namespace Corona;
    Systems::UI::SdlWindowManager manager;
    auto& commands = DisplaySurfaceCommandChannel::instance().commands();
    SDL_Window missing{1, nullptr};
    if (manager.adopt_platform_window(nullptr) || manager.adopt_platform_window(&missing)) return 1;
    if (!commands.drain().empty()) return 2;
    SDL_Window window{2, reinterpret_cast<void*>(42)};
    // No Display consumer is running: native surface creation can itself need
    // the UI thread to process messages. Adoption must only enqueue and return.
    if (!manager.adopt_platform_window(&window)) {
        std::cerr << "platform adoption rejected a pending Display registration";
        return 3;
    }
    auto pending = commands.drain();
    if (pending.size() != 1 || pending[0].kind != DisplaySurfaceCommand::Kind::Changed ||
        pending[0].surface != window.surface) return 4;
    // Registration alone must not reveal the window; first-present owns that.
    const auto* managed = manager.find_by_surface(window.surface);
    if (!managed || !managed->pending_show) return 5;
}
''', encoding='utf-8')
    # Link the real lookup and completion tickets too; only OS access is stubbed.
    lookup_start = source.index('const ManagedWindow* SdlWindowManager::find_by_surface(')
    lookup_end = source.index('\n}', lookup_start) + 2
    with harness.open('a', encoding='utf-8') as output:
        output.write('\nnamespace Corona::Systems::UI {\n' + source[lookup_start:lookup_end] + '\n}\n')
    executable = tmp_path / 'registration.exe'
    built = subprocess.run([compiler, '-std=c++20', '-I' + str(tmp_path),
                            '-I' + str(ROOT / 'include'), str(harness),
                            str(ROOT / 'src/systems/ui/ui_surface_lifecycle.cpp'),
                            '-o', str(executable)], capture_output=True, text=True)
    assert built.returncode == 0, built.stdout + built.stderr
    result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
