import os

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.files import copy, get


class SlangConan(ConanFile):
    name = "slang"
    version = "2026.11"
    package_type = "shared-library"
    license = "Apache-2.0 WITH LLVM-exception"
    homepage = "https://github.com/shader-slang/slang"
    description = "Slang shading language compiler and runtime binary SDK."
    settings = "os", "arch"

    _assets = {
        ("Windows", "x86_64"): (
            "windows-x86_64",
            "e070e239290108dd7a83094cf72fb94ae753b2d5331adba76874bf17dd5c8b6a",
        ),
        ("Windows", "armv8"): (
            "windows-aarch64",
            "8380746b38cff5c727f7ff6a73a9d9608eee7230a886540244b977af3596d8ec",
        ),
    }

    def _asset(self):
        return self._assets.get((str(self.settings.os), str(self.settings.arch)))

    def validate(self):
        if not self._asset():
            raise ConanInvalidConfiguration(
                f"slang/{self.version} binary SDK is only configured for Windows x86_64 and armv8"
            )

    def package(self):
        asset_suffix, sha256 = self._asset()
        asset_name = f"slang-{self.version}-{asset_suffix}.zip"
        sdk_dir = os.path.join(self.build_folder, "sdk")
        get(
            self,
            url=f"{self.homepage}/releases/download/v{self.version}/{asset_name}",
            sha256=sha256,
            destination=sdk_dir,
            strip_root=False,
        )

        copy(self, "LICENSE", src=sdk_dir, dst=os.path.join(self.package_folder, "licenses"), keep_path=False)
        copy(self, "*", src=os.path.join(sdk_dir, "include"), dst=os.path.join(self.package_folder, "include"), keep_path=True)
        copy(self, "*", src=os.path.join(sdk_dir, "lib"), dst=os.path.join(self.package_folder, "lib"), keep_path=True)
        copy(self, "*", src=os.path.join(sdk_dir, "bin"), dst=os.path.join(self.package_folder, "bin"), keep_path=True)

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "slang")
        self.cpp_info.set_property("cmake_target_name", "slang::slang_package")
        self.cpp_info.bindirs = ["bin"]
        self.cpp_info.libdirs = ["lib"]
        self.cpp_info.includedirs = ["include"]

        self.cpp_info.components["slang"].set_property("cmake_target_name", "slang::slang")
        self.cpp_info.components["slang"].libs = ["slang"]
        self.cpp_info.components["slang"].defines = ["SLANG_DYNAMIC"]

        self.cpp_info.components["slang-rt"].set_property("cmake_target_name", "slang::slang-rt")
        self.cpp_info.components["slang-rt"].libs = ["slang-rt"]

        self.cpp_info.components["gfx"].set_property("cmake_target_name", "slang::gfx")
        self.cpp_info.components["gfx"].libs = ["gfx"]
        self.cpp_info.components["gfx"].defines = ["SLANG_GFX_DYNAMIC"]
        self.cpp_info.components["gfx"].requires = ["slang"]

        self.cpp_info.components["slang-compiler"].set_property("cmake_target_name", "slang::slang-compiler")
        self.cpp_info.components["slang-compiler"].libs = ["slang-compiler"]
        self.cpp_info.components["slang-compiler"].requires = ["slang"]

        self.runenv_info.prepend_path("PATH", os.path.join(self.package_folder, "bin"))
