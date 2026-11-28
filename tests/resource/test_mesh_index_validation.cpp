#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <string>
#include <vector>

#include "resource/types/parse_common.h"

using namespace Corona::Resource;

namespace {

int g_passed = 0;
int g_failed = 0;

void check(bool cond, const char* name) {
    if (cond) {
        ++g_passed;
        std::printf("[PASS] %s\n", name);
    } else {
        ++g_failed;
        std::printf("[FAIL] %s\n", name);
    }
}

bool message_contains(const std::exception& e, const std::string& needle) {
    return std::string(e.what()).find(needle) != std::string::npos;
}

void test_convert_indices_rejects_uint16_overflow() {
    const std::vector<std::uint32_t> indices{
        0,
        1,
        static_cast<std::uint32_t>(std::numeric_limits<std::uint16_t>::max()) + 1u,
    };

    bool threw_expected = false;
    try {
        (void)convert_indices_to_uint16(indices, 70000, "overflow_mesh");
    } catch (const std::exception& e) {
        threw_expected = message_contains(e, "overflow_mesh") &&
                         message_contains(e, "65536");
    }

    check(threw_expected, "convert_indices_to_uint16 rejects indices that would truncate");
}

void test_convert_indices_rejects_vertex_count_overflow() {
    const std::vector<std::uint32_t> indices{0, 1, 2};

    bool threw_expected = false;
    try {
        (void)convert_indices_to_uint16(indices, 70000, "too_many_vertices");
    } catch (const std::exception& e) {
        threw_expected = message_contains(e, "too_many_vertices") &&
                         message_contains(e, "70000");
    }

    check(threw_expected, "convert_indices_to_uint16 rejects vertex counts above uint16 range");
}

void test_convert_indices_accepts_valid_range() {
    const std::vector<std::uint32_t> indices{0, 2, 1};
    const auto converted = convert_indices_to_uint16(indices, 3, "valid_mesh");

    check(converted.size() == indices.size() &&
              converted[0] == 0 &&
              converted[1] == 2 &&
              converted[2] == 1,
          "convert_indices_to_uint16 preserves valid indices");
}

}  // namespace

int main() {
    std::printf("=== Mesh Index Validation Tests ===\n");

    test_convert_indices_rejects_uint16_overflow();
    test_convert_indices_rejects_vertex_count_overflow();
    test_convert_indices_accepts_valid_range();

    std::printf("\n=== Results: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
