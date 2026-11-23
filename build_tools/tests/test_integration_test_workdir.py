"""Configure the production GPU test registration without optional examples."""
from pathlib import Path
import shutil
import subprocess

import pytest


def test_restir_winner_target_configures_without_engine_example(tmp_path):
    cmake = shutil.which("cmake")
    if not cmake:
        pytest.skip("Test registration regression requires CMake")
    root = Path(__file__).resolve().parents[2]
    source = (root / "tests/integration/CMakeLists.txt").read_text(encoding="utf-8")
    start = source.index("add_test(NAME VisionReSTIRWinnerTargetTests")
    end = source.index("add_test(NAME VisionPrincipledStablePlanes", start)
    (tmp_path / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.20)\n'
        'project(TestRegistration LANGUAGES NONE)\n'
        'enable_testing()\n'
        'add_executable(corona_vision_restir_estimator_tests IMPORTED)\n'
        'set_target_properties(corona_vision_restir_estimator_tests PROPERTIES\n'
        '    IMPORTED_LOCATION "${CMAKE_BINARY_DIR}/runtime/estimator.exe")\n'
        'add_library(vision-material-principled_bsdf MODULE IMPORTED)\n'
        'set_target_properties(vision-material-principled_bsdf PROPERTIES\n'
        '    IMPORTED_LOCATION "${CMAKE_BINARY_DIR}/runtime/material.dll")\n'
        + source[start:end], encoding="utf-8",
    )
    build = tmp_path / "build"
    result = subprocess.run([cmake, "-S", str(tmp_path), "-B", str(build)],
                            capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stdout + result.stderr
    registration = (build / "CTestTestfile.cmake").read_text(encoding="utf-8")
    assert f'WORKING_DIRECTORY "{build.as_posix()}/runtime"' in registration
    assert 'VISION_TEST_GI_WINNER_TARGET_ONLY=1' in registration
