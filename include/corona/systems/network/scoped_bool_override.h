#pragma once

namespace Corona::Network {

class ScopedBoolOverride {
public:
    ScopedBoolOverride(bool& target, bool value)
        : target_(target), previous_(target) {
        target_ = value;
    }

    ~ScopedBoolOverride() { target_ = previous_; }

    ScopedBoolOverride(const ScopedBoolOverride&) = delete;
    ScopedBoolOverride& operator=(const ScopedBoolOverride&) = delete;

private:
    bool& target_;
    bool previous_;
};

}  // namespace Corona::Network
