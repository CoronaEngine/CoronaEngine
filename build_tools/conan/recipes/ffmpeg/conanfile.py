import os

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.files import copy, get


class FFmpegConan(ConanFile):
    name = "ffmpeg"
    version = "8.1.1"
    package_type = "shared-library"
    license = "LGPL-2.1-or-later"
    homepage = "https://github.com/BtbN/FFmpeg-Builds"
    description = "Pinned BtbN FFmpeg LGPL shared binary package for Windows."
    settings = "os", "arch"
    options = {"shared": [True, False]}
    default_options = {"shared": True}

    _tag = "autobuild-2026-05-31-13-22"
    _archive_name = "ffmpeg-N-124714-g49a77d37be-win64-lgpl-shared.zip"
    _sha256 = "56f4a1d367e9537f63849e5cf9103824f6d87f4fc39a6a22b717b4df186da054"
    _components = ("avutil", "avcodec", "avformat", "swscale", "swresample")

    def validate(self):
        if self.settings.os != "Windows" or self.settings.arch != "x86_64":
            raise ConanInvalidConfiguration("This ffmpeg recipe packages the pinned Windows x86_64 BtbN build only")
        if not self.options.shared:
            raise ConanInvalidConfiguration("The pinned BtbN FFmpeg package is shared-only")

    def package(self):
        ffmpeg_dir = os.path.join(self.build_folder, "ffmpeg")
        get(
            self,
            url=f"{self.homepage}/releases/download/{self._tag}/{self._archive_name}",
            sha256=self._sha256,
            destination=ffmpeg_dir,
            strip_root=True,
        )
        copy(self, "*", src=ffmpeg_dir, dst=self.package_folder, keep_path=True)

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "ffmpeg")
        self.cpp_info.includedirs = []
        self.cpp_info.libdirs = []
        self.cpp_info.bindirs = []

        for component_name in self._components:
            component = self.cpp_info.components[component_name]
            component.set_property("cmake_target_name", f"ffmpeg::{component_name}")
            component.includedirs = ["include"]
            component.libdirs = ["lib"]
            component.bindirs = ["bin"]
            component.libs = [component_name]

        self.cpp_info.components["avcodec"].requires = ["avutil"]
        self.cpp_info.components["avformat"].requires = ["avcodec", "avutil"]
        self.cpp_info.components["swscale"].requires = ["avutil"]
        self.cpp_info.components["swresample"].requires = ["avutil"]
