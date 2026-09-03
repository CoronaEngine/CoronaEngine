import os

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout

required_conan_version = ">=2.28"


class CoronaEngineConan(ConanFile):
    name = "coronaengine"
    package_type = "library"
    settings = "os", "arch", "compiler", "build_type"
    _target_families = (
        "core",
        "examples",
        "tests",
        "vision",
        "vision-tests",
        "vision-oidn",
    )

    options = {
        "shared": [True, False],
        "with_editor": [True, False],
        "with_examples": [True, False],
        "with_tests": [True, False],
        "with_vision": [True, False],
        "with_vision_tests": [True, False],
        "with_oidn": [True, False],
        "with_cef": [True, False],
    }

    default_options = {
        "shared": False,
        "with_editor": True,
        "with_examples": True,
        "with_tests": False,
        "with_vision": True,
        "with_vision_tests": False,
        "with_oidn": False,
        "with_cef": True,
        "sdl/*:shared": False,
        "glfw/*:shared": False,
        "volk/*:shared": False,
        "spirv-cross/*:shared": False,
        "spirv-cross/*:build_executable": False,
        "spirv-tools/*:shared": False,
        "spirv-tools/*:build_executables": False,
        "ffmpeg/*:shared": True,
        "hwloc/*:shared": True,
    }

    def layout(self):
        configuration = str(self.settings.build_type).lower()
        target_family = self.conf.get("user.corona:target_family", default="examples")
        if target_family not in self._target_families:
            raise ConanInvalidConfiguration(
                f"Unsupported user.corona:target_family='{target_family}'. "
                f"Expected one of: {', '.join(self._target_families)}"
            )
        cmake_layout(self, build_folder=f"build/conan/{target_family}/{configuration}")

    def set_version(self):
        self.version = os.environ.get("CORONAENGINE_CONAN_VERSION", "0.5.0")

    def requirements(self):
        self.requires("ktm/0.2.14", transitive_headers=True)
        self.requires("pfr/1.91.0", transitive_headers=True)
        self.requires("spirv-cross/1.4.350.0", transitive_headers=True, transitive_libs=True)
        self.requires("spirv-tools/1.4.350.0", transitive_headers=True, transitive_libs=True)
        self.requires("volk/1.4.350.0", transitive_headers=True, transitive_libs=True)
        self.requires("vulkan-headers/1.4.350.0", transitive_headers=True)
        self.requires("vulkan-memory-allocator/3.4.0", transitive_headers=True)
        self.requires("quill/11.0.2", transitive_headers=True, transitive_libs=True)
        self.requires("slang/2026.10", transitive_headers=True, transitive_libs=True)
        self.requires("tracy/0.13.1", options={"on_demand": True})
        # Required by Horizon core (as of commit 5b4d13fa)
        self.requires("fmt/12.1.0", transitive_headers=True, transitive_libs=True)
        self.requires("spdlog/1.17.0", transitive_headers=True, transitive_libs=True)
        self.requires("xxhash/0.8.3", transitive_headers=True, transitive_libs=True)
        self.requires("assimp/5.4.3", transitive_headers=True, transitive_libs=True)
        self.requires("stb/cci.20230920", transitive_headers=True)
        self.requires("nanobind/2.9.2", transitive_headers=True, transitive_libs=True)
        self.requires("sdl/3.4.0", transitive_headers=True, transitive_libs=True)
        self.requires("enet/1.3.18", transitive_headers=True, transitive_libs=True)
        self.requires("onetbb/2022.3.0", transitive_headers=True, transitive_libs=True)
        self.requires("miniaudio/0.11.21", transitive_headers=True)
        self.requires("nlohmann_json/3.12.0", transitive_headers=True)
        self.requires("tinyexr/1.0.7", transitive_headers=True, transitive_libs=True)
        self.requires("meshoptimizer/0.25", transitive_headers=True, transitive_libs=True)
        self.requires("astc-encoder/5.3.0", transitive_headers=True, transitive_libs=True)
        self.requires("ffmpeg/8.1.1", transitive_headers=True, transitive_libs=True)

        if bool(self.options.with_cef):
            self.requires("cef-binary/143.0.14.gdd46a37.chromium143.0.7499.193",
                          transitive_headers=True, transitive_libs=True)

        if bool(self.options.with_vision):
            self.requires("cxxopts/3.2.0", transitive_headers=True)
            self.requires("glfw/3.4", transitive_headers=True, transitive_libs=True)
            if bool(self.options.with_oidn):
                self.requires("openimagedenoise/2.3.3", transitive_headers=True, transitive_libs=True)

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()

        toolchain = CMakeToolchain(self)
        # Family-specific generators each export a conan-default preset. The
        # checked-in presets are the single public entrypoint, so do not merge
        # generated presets into the repository-root CMakeUserPresets.json.
        toolchain.user_presets_path = None
        variables = toolchain.variables
        variables["BUILD_SHARED_LIBS"] = bool(self.options.shared)
        variables["BUILD_CORONA_EDITOR"] = bool(self.options.with_editor)
        variables["BUILD_CORONA_EXAMPLES"] = bool(self.options.with_examples)
        variables["BUILD_CORONA_TESTING"] = bool(self.options.with_tests)
        variables["CORONA_BUILD_VISION"] = bool(self.options.with_vision)
        variables["VISION_BUILD_TESTS"] = bool(self.options.with_vision_tests)
        variables["VISION_BUILD_OIDN"] = bool(self.options.with_oidn)
        variables["CORONA_ENABLE_CEF"] = bool(self.options.with_cef)

        if bool(self.options.with_cef):
            cef_dep = self.dependencies["cef-binary"]
            variables["CORONA_CEF_ROOT"] = cef_dep.package_folder.replace("\\", "/")

        toolchain.generate()

    def validate(self):
        if bool(self.options.with_vision_tests) and not bool(self.options.with_vision):
            raise ConanInvalidConfiguration("with_vision_tests=True requires with_vision=True")
        if bool(self.options.with_oidn) and not bool(self.options.with_vision):
            raise ConanInvalidConfiguration("with_oidn=True requires with_vision=True")

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build(target="CoronaEngine")
