#include "cef/scene_folder.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

namespace fs = std::filesystem;
using namespace Corona::Systems::UI::SceneFolders;

namespace {

void expect(bool condition, std::string_view message) {
    if (condition) return;
    std::cerr << "VisionOutputPathsTests failed: " << message << '\n';
    std::exit(1);
}

void write_text(const fs::path& path, std::string_view value) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << value;
}

void output_filename_is_not_validated_as_a_resource() {
    const auto scene = fs::temp_directory_path() / "corona_vision_output_paths_tests";
    std::error_code ec;
    fs::remove_all(scene, ec);
    expect(create_scene_folder(scene, "Vision output").has_value(),
           "portable scene should be created");
    write_text(scene / "scene.ini",
               "[format]\ntype = corona_scene_folder\nversion = 1\n"
               "[scene]\nname = Vision output\n"
               "[vision]\nstorage = embedded\n"
               "[vision_document]\nversion = 1\nencoding = zlib_base64_json\nasset_root = Assets\n"
               "data = eAEBIQDe/3sib3V0cHV0Ijp7ImZuIjoiaGVybzEtQ1BVLnBuZyJ9fb+PCx0=\n");

    const auto validation = validate_portable_scene(scene, true);
    expect(validation.ok(), "Vision output filename must not be validated as an input asset");
    expect(is_vision_output_section_key("Output"),
           "Vision output section matching should be case-insensitive");
    fs::remove_all(scene, ec);
}

}  // namespace

int main() {
    output_filename_is_not_validated_as_a_resource();
    return 0;
}
