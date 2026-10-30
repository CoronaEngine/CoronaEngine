from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy
from conan.tools.scm import Git


class AstcEncoderConan(ConanFile):
    name = "astc-encoder"
    version = "5.3.0"
    package_type = "static-library"
    license = "Apache-2.0"
    homepage = "https://github.com/ARM-software/astc-encoder"
    description = "ASTC texture compressor library."
    settings = "os", "arch", "compiler", "build_type"

    def layout(self):
        cmake_layout(self)

    def source(self):
        git = Git(self)
        git.clone(url=self.homepage, target=".")
        git.checkout(self.version)

    def generate(self):
        toolchain = CMakeToolchain(self)
        toolchain.cache_variables["ASTCENC_CLI"] = False
        toolchain.cache_variables["ASTCENC_UNITTEST"] = False
        toolchain.cache_variables["ASTCENC_SHAREDLIB"] = False
        toolchain.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build(target="astcenc-native-static")

    def package(self):
        copy(self, "LICENSE.txt", src=self.source_folder, dst=self.package_folder, keep_path=False)
        copy(self, "astcenc*.h", src=f"{self.source_folder}/Source", dst=f"{self.package_folder}/include", keep_path=False)
        copy(self, "*.lib", src=self.build_folder, dst=f"{self.package_folder}/lib", keep_path=False)
        copy(self, "*.a", src=self.build_folder, dst=f"{self.package_folder}/lib", keep_path=False)

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "astc-encoder")
        self.cpp_info.set_property("cmake_target_name", "astcenc-native-static")
        self.cpp_info.libs = ["astcenc-native-static"]
