import os

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.files import copy, get


class CefBinaryConan(ConanFile):
    name = "cef-binary"
    version = "143.0.14.gdd46a37.chromium143.0.7499.193"
    package_type = "shared-library"
    license = "BSD-3-Clause"
    homepage = "https://cef-builds.spotifycdn.com"
    description = "Chromium Embedded Framework binary distribution."
    settings = "os", "arch"

    _version_label = "143.0.14+gdd46a37+chromium-143.0.7499.193"
    _assets = {
        ("Windows", "x86_64"): (
            "windows64",
            "0fa946d0f3834165632483fa3a3c2744aaa2d69862697953766293100d2d8b1d",
        ),
    }

    def _asset(self):
        return self._assets.get((str(self.settings.os), str(self.settings.arch)))

    def validate(self):
        if not self._asset():
            raise ConanInvalidConfiguration("cef-binary is currently configured for Windows x86_64 only")

    def package(self):
        platform, sha256 = self._asset()
        archive = f"cef_binary_{self._version_label}_{platform}.tar.bz2"
        cef_dir = os.path.join(self.build_folder, "cef")
        get(
            self,
            url=f"{self.homepage}/{archive}",
            sha256=sha256,
            destination=cef_dir,
            strip_root=True,
        )
        copy(self, "*", src=cef_dir, dst=self.package_folder, keep_path=True)

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "cef-binary")
        self.cpp_info.set_property("cmake_target_name", "cef_binary::cef_binary")
        self.cpp_info.includedirs = ["include"]
        self.cpp_info.libdirs = [os.path.join("Release"), os.path.join("Debug")]
        self.cpp_info.bindirs = [os.path.join("Release"), os.path.join("Debug")]
