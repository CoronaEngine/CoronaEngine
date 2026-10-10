"""Windows integration check for the shared CLion configure/build environment."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from tools.build.msvc_showincludes import dependency_prefixes, normalize_line


ROOT = Path(__file__).resolve().parents[2]


class ShowIncludesTests(unittest.TestCase):
    def test_localized_dependencies_survive_both_code_page_directions(self):
        prefix = "注意: 包含文件:  "
        path = "D:\\工程\\材质.h\r\n"
        for configure_encoding in ("utf-8", "cp936"):
            prefixes = dependency_prefixes(prefix.encode(configure_encoding), "cp936")
            for build_encoding in ("utf-8", "cp936"):
                with self.subTest(configure=configure_encoding, build=build_encoding):
                    line = (prefix + "  " + path).encode(build_encoding)
                    self.assertEqual(normalize_line(line, prefixes),
                                     b"Note: including file: " + path.encode("utf-8"))

    def test_english_dependency_and_diagnostic_output(self):
        prefixes = dependency_prefixes(b"Note: including file: ", "cp936")
        self.assertEqual(normalize_line(b"Note: including file:   D:\\header.h\r\n", prefixes),
                         b"Note: including file: D:\\header.h\r\n")
        diagnostic = "源文件.cpp(1): error C2065: identifier\r\n".encode("cp936")
        self.assertEqual(normalize_line(diagnostic, prefixes), diagnostic)

    def test_english_prefix_with_legacy_non_ascii_path(self):
        prefixes = dependency_prefixes(b"Note: including file: ", "cp936")
        path = "D:\\工程\\材质.h\r\n"
        self.assertEqual(
            normalize_line(b"Note: including file: " + path.encode("cp936"), prefixes),
            b"Note: including file: " + path.encode("utf-8"),
        )


@unittest.skipUnless(os.name == "nt", "requires the saved Windows CLion toolchain")
class ClionMsvcEncodingTests(unittest.TestCase):
    def test_header_edit_rebuilds_after_legacy_console_start(self):
        cache = ROOT / "cmake-build-relwithdebinfo/CMakeCache.txt"
        if not cache.exists():
            self.skipTest("requires the configured CLion profile")
        values = {}
        for line in cache.read_text(encoding="utf-8").splitlines():
            if "=" in line and not line.startswith(("#", "//")):
                key, value = line.split("=", 1)
                values[key.split(":", 1)[0]] = value

        def quote(value):
            return "'" + str(value).replace("'", "''") + "'"

        with tempfile.TemporaryDirectory(prefix="corona-msvc-工程 ") as directory:
            source = Path(directory)
            (source / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.29)\n"
                "project(HeaderDependencyProbe LANGUAGES CXX)\n"
                f'set(Python_EXECUTABLE "{Path(sys.executable).as_posix()}")\n'
                f'include("{ROOT.as_posix()}/cmake/corona_msvc_dependencies.cmake")\n'
                "add_executable(probe main.cpp)\n",
                encoding="utf-8",
            )
            (source / "main.cpp").write_text(
                '#include "value.h"\nint main() { return PROBE_VALUE; }\n',
                encoding="ascii",
            )
            (source / "value.h").write_text("#define PROBE_VALUE 0\n", encoding="ascii")
            helper = ROOT / ".agents/skills/clion-cmake-relwithdebinfo/scripts/clion-msvc.ps1"
            script = source / "check.ps1"
            script.write_text(
                "$ErrorActionPreference = 'Stop'\n"
                "[Console]::OutputEncoding = [Text.Encoding]::GetEncoding(936)\n"
                f". {quote(helper)}\n"
                f"Import-ClionMsvcEnvironment -RepositoryRoot {quote(ROOT)} "
                f"-CMakePath {quote(values['CMAKE_COMMAND'])}\n"
                f"& {quote(values['CMAKE_COMMAND'])} -S {quote(source)} "
                f"-B {quote(source / 'build')} -G Ninja "
                f"{quote('-DCMAKE_MAKE_PROGRAM=' + values['CMAKE_MAKE_PROGRAM'])}\n"
                "if ($LASTEXITCODE) { exit $LASTEXITCODE }\n"
                "[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)\n"
                f"& {quote(values['CMAKE_COMMAND'])} --build {quote(source / 'build')}\n"
                "if ($LASTEXITCODE) { exit $LASTEXITCODE }\n"
                f"& {quote(source / 'build/probe.exe')}\n"
                "if ($LASTEXITCODE -ne 0) { throw 'initial compile failed' }\n"
                f"Set-Content -Encoding ascii -LiteralPath {quote(source / 'value.h')} "
                "-Value '#define PROBE_VALUE 7'\n"
                f"& {quote(values['CMAKE_COMMAND'])} --build {quote(source / 'build')}\n"
                "if ($LASTEXITCODE) { exit $LASTEXITCODE }\n"
                f"& {quote(source / 'build/probe.exe')}\n"
                "if ($LASTEXITCODE -ne 7) { throw 'header change did not rebuild the executable' }\n"
                "exit 0\n",
                encoding="utf-8-sig",
            )
            result = subprocess.run(
                ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120,
            )
            rules = source / "build/CMakeFiles/rules.ninja"
            prefix = ([line for line in rules.read_bytes().splitlines()
                       if line.startswith(b"msvc_deps_prefix")] if rules.exists() else [])
            self.assertEqual(result.returncode, 0,
                             repr(prefix) + "\n" + result.stdout.decode("utf-8", errors="replace"))


if __name__ == "__main__":
    unittest.main()
