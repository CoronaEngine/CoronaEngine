#include "core/util/switch_profile.h"
#include <stdexcept>

namespace {
int early_return(bool stop) {
    ocarina::switch_profile::Scope profile{"early_return", "prepare"};
    if (stop) return 7;
    return 0;
}

void unwind() {
    ocarina::switch_profile::Scope profile{"unwind", "prepare"};
    throw std::runtime_error("expected");
}
}

int main() {
    ocarina::switch_profile::Scope profile{"PT_to_ReSTIR", "switch"};
    {
        ocarina::switch_profile::Scope nested{"nested", "prepare"};
        if (early_return(true) != 7) return 1;
        try {
            unwind();
            return 2;
        } catch (const std::runtime_error&) {
        }
    }
    // A sibling must not be incorrectly nested under the unwound function.
    ocarina::switch_profile::Scope sibling{"sibling", "prepare"};
    return 0;
}
