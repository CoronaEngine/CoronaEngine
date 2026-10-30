"""Configure without FindVulkan and deploy the Conan DLL after CEF's copy."""

import os
import shutil
import subprocess

import pytest

from build_tools.paths import REPO_ROOT
from build_tools import workflow


@pytest.fixture
def project(tmp_path):
    cmake = shutil.which("cmake")
    ninja = shutil.which("ninja") or workflow.cached_ninja(REPO_ROOT, "RelWithDebInfo", "core")
    module = REPO_ROOT / "Horizon/cmake/horizon_vulkan.cmake"
    if not cmake or not ninja or not module.is_file():
        pytest.skip("requires CMake, Ninja and the Horizon source workspace")
    source = tmp_path / "source"
    source.mkdir()
    package = source / "package"
    package.mkdir()
    (source / "FindVulkan.cmake").write_text('message(FATAL_ERROR "SDK lookup attempted")\n')
    preamble = (
        'cmake_minimum_required(VERSION 3.29)\n'
        'set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}")\n'
        'set(CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/package")\n'
        'set(CMAKE_FIND_USE_PACKAGE_REGISTRY FALSE)\n'
        'set(CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY FALSE)\n'
    )
    return cmake, source, package, module.as_posix(), preamble, ninja


@pytest.mark.parametrize("missing", ["package", "bin-directory", "dll"])
def test_missing_conan_loader_fails_without_sdk_fallback(project, missing):
    cmake, source, package, module, preamble, ninja = project
    if missing != "package":
        config = 'add_library(Vulkan::Loader INTERFACE IMPORTED)\n'
        if missing == "dll":
            config += 'set(vulkan-loader_BIN_DIRS_RELEASE "${CMAKE_CURRENT_LIST_DIR}/missing")\n'
        (package / "VulkanLoaderConfig.cmake").write_text(config)
    (source / "CMakeLists.txt").write_text(
        preamble + 'project(LoaderMissing LANGUAGES NONE)\nset(WIN32 TRUE)\n'
        + f'include("{module}")\n'
    )
    result = subprocess.run(
        [cmake, "-S", str(source), "-B", str(source / "build"),
         "-G", "Ninja Multi-Config", f"-DCMAKE_MAKE_PROGRAM={ninja}",
         "-DCMAKE_DISABLE_FIND_PACKAGE_Vulkan=TRUE"],
        capture_output=True, text=True, timeout=60,
    )
    output = result.stdout + result.stderr
    assert result.returncode != 0
    expected = {"package": "VulkanLoader", "bin-directory": "runtime bin directory",
                "dll": "loader runtime is missing"}[missing]
    assert expected in output
    assert "SDK lookup attempted" not in output


@pytest.mark.skipif(os.name != "nt", reason="Windows DLL deployment")
def test_loader_configuration_and_cef_copy_order(project):
    cmake, source, package, module, preamble, ninja = project
    env = dict(os.environ)
    if not shutil.which("cl"):
        if not (workflow.generators_dir(REPO_ROOT, "RelWithDebInfo", "core") / "conanbuild.bat").is_file():
            pytest.skip("requires an MSVC environment or an installed core Conan profile")
        env = workflow.load_conan_build_environment(REPO_ROOT, "RelWithDebInfo", "core")
    for key in list(env):
        if key.upper() in {"VULKAN_SDK", "VK_SDK_PATH"}:
            del env[key]
        elif key.upper() == "PATH":
            env[key] = os.pathsep.join(p for p in env[key].split(os.pathsep) if "vulkansdk" not in p.lower())

    configs = ("Debug", "RelWithDebInfo")
    config_text = 'add_library(Vulkan::Loader INTERFACE IMPORTED)\n'
    for config in configs:
        (package / config).mkdir()
        (package / config / "vulkan-1.dll").write_text(f"Conan {config}")
        config_text += f'set(vulkan-loader_BIN_DIRS_{config.upper()} "${{CMAKE_CURRENT_LIST_DIR}}/{config}")\n'
    (package / "VulkanLoaderConfig.cmake").write_text(config_text)
    for config in ("Debug", "Release"):
        (source / "cef" / config).mkdir(parents=True)
        (source / "cef" / config / "vulkan-1.dll").write_text("CEF loader must be replaced")
    resources = source / "cef/Resources"
    (resources / "locales").mkdir(parents=True)
    for name in ("data.pak", "locales/en-US.pak", "locales/zh-CN.pak"):
        (resources / name).write_text("CEF resource")
    (source / "python-runtime.dll").write_text("Python runtime fixture")
    (source / "main.c").write_text("int main(void) { return 0; }\n")
    runtime_module = (REPO_ROOT / "build_tools/cmake/corona_runtime_deps.cmake").as_posix()
    (source / "CMakeLists.txt").write_text(
        preamble + 'project(LoaderDeployment LANGUAGES C)\n'
        + f'include("{module}")\ninclude("{runtime_module}")\n'
        + 'add_library(CoronaEngine INTERFACE)\n'
        'set_property(TARGET CoronaEngine PROPERTY INTERFACE_CORONA_RUNTIME_DEPS "${CMAKE_CURRENT_SOURCE_DIR}/python-runtime.dll")\n'
        'set(CORONA_ENABLE_CEF ON)\nset(CEF_AVAILABLE ON)\n'
        'set(CEF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/cef")\n'
        'add_executable(probe main.c)\n'
        'target_link_libraries(probe PRIVATE Vulkan::Loader)\n'
        'corona_install_runtime_deps(probe)\n'
    )
    build = source / "build"
    result = subprocess.run(
        [cmake, "-S", str(source), "-B", str(build), "-G", "Ninja Multi-Config",
         f"-DCMAKE_MAKE_PROGRAM={ninja}", "-DCMAKE_C_COMPILER=cl",
         "-DCMAKE_DISABLE_FIND_PACKAGE_Vulkan=TRUE"],
        env=env, capture_output=True, text=True, timeout=60,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    for config in configs:
        command = [cmake, "--build", str(build), "--config", config]
        result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=60)
        assert result.returncode == 0, result.stdout + result.stderr
        deployed = build / config / "vulkan-1.dll"
        assert deployed.read_text() == f"Conan {config}"
        assert (build / config / "locales/en-US.pak").is_file()
        # A replacement package must refresh the copied DLL without touching code.
        (package / config / "vulkan-1.dll").write_text(f"Updated Conan {config}")
        result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=60)
        assert result.returncode == 0, result.stdout + result.stderr
        assert deployed.read_text() == f"Updated Conan {config}"
