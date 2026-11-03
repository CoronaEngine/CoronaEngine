from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import copy, get


class CoronaImGuiConan(ConanFile):
    name = "corona-imgui"
    version = "1.92.5"
    package_type = "static-library"
    license = "MIT"
    homepage = "https://github.com/ocornut/imgui"
    description = "Pinned Dear ImGui docking core and SDL3 platform backend; rendering is owned by Corona."
    settings = "os", "arch", "compiler", "build_type"
    exports_sources = "CMakeLists.txt"

    def requirements(self):
        self.requires("sdl/3.4.0")

    def layout(self):
        cmake_layout(self)

    def source(self):
        get(self,
            url="https://codeload.github.com/ocornut/imgui/zip/refs/tags/v1.92.5-docking",
            sha256="c893a95aa68e8e7380ca7868adfb2b19c45b9c6e6777a3bdd945c945972c2264",
            strip_root=True)

    def generate(self):
        CMakeDeps(self).generate()
        CMakeToolchain(self).generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        CMake(self).install()
        copy(self, "LICENSE.txt", src=self.source_folder, dst=f"{self.package_folder}/licenses")

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "corona-imgui")
        self.cpp_info.set_property("cmake_target_name", "corona::imgui")
        self.cpp_info.libs = ["corona-imgui"]
        self.cpp_info.includedirs = ["include", "include/backends"]
        if self.settings.os == "Windows":
            self.cpp_info.system_libs = ["imm32", "user32", "shell32"]
