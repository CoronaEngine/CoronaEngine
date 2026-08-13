#pragma once

#include <filesystem>
#include <string>

namespace Corona::Script::Python::PathCfg {

// All runtime paths are resolved relative to the running executable. This is
// intentional: the executable may be started from any working directory and
// the whole build output directory may be moved after it is packaged.
const std::filesystem::path& executable_dir();
const std::filesystem::path& engine_root_path();

const std::string& engine_root();
const std::string& editor_backend_rel();
const std::string& editor_backend_abs();
std::string runtime_backend_abs();
std::string python_home_dir();
std::string python_stdlib_zip();
std::string python_dll_dir();
std::string python_lib_dir();
std::string site_packages_dir();

}  // namespace Corona::Script::Python::PathCfg
