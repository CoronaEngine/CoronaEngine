#include <corona/systems/network/collaboration_state.h>
#include <stdexcept>

void check(bool condition) {
    if (!condition) throw std::runtime_error("object collaboration regression");
}

int main() {
    Corona::Network::CollaborationState state;
    check(!state.lock_object("", "user", "modify", 0).ok);
    check(state.lock_object("actor", "host", "modify", 100).ok);
    check(!state.lock_object("actor", "guest", "modify", 200).ok);
    check(!state.unlock_object("actor", "guest").ok);
    check(state.locked_by("actor", 300) == "host");
    check(state.locked_by("actor", 30100).empty());
    check(state.lock_object("actor", "guest", "modify", 30100).ok);
    check(state.unlock_object("actor", "guest").ok);
    state.broadcast_intent("host", "placing", {1, 0, 1}, "preview", 100);
    check(state.check_preview_collision("guest", {1, 100, 1}, .5f, 101) == "host");
    check(state.check_preview_collision("host", {1, 0, 1}, .5f, 101).empty());
    check(state.check_preview_collision("guest", {3, 0, 3}, .5f, 101).empty());
    check(state.check_preview_collision("guest", {1, 0, 1}, .5f, 60100).empty());
    state.lock_object("actor", "host", "modify", 60100);
    state.clear();
    check(state.locked_by("actor", 60101).empty());
}
