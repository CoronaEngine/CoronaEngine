#include "common/diagnostic_env.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void expect(bool condition, std::string_view message) {
    if (!condition) fail(message);
}

void diagnostic_flags_default_off_and_accept_truthy_values() {
    using Corona::Systems::Diagnostics::parse_env_flag;
    expect(!parse_env_flag(nullptr), "missing flag must be disabled");
    expect(!parse_env_flag(""), "empty flag must be disabled");
    expect(!parse_env_flag("0"), "zero flag must be disabled");
    expect(!parse_env_flag("OFF"), "off flag must be case insensitive");
    expect(parse_env_flag("1"), "one flag must be enabled");
    expect(parse_env_flag("yes"), "yes flag must be enabled");
}

}  // namespace

int main() {
    diagnostic_flags_default_off_and_accept_truthy_values();
    std::cout << "Diagnostic flag tests passed\n";
    return 0;
}
