"""Locate a C++ compiler for the standalone CPU regressions.

These tests compile a slice of real project source against stubs and run it, so they need a
compiler that g++ or clang++ provides. On this machine MinGW is installed outside PATH, which
made every one of them skip silently - and a skipped regression proves nothing. Probe PATH
first, then the usual installation directories, and put the compiler's own directory on PATH
because the linked executables need the runtime DLLs at run time.
"""

from pathlib import Path
import os
import shutil


_INSTALLED = (
    "C:/mingw64/bin/g++.exe",
    "C:/msys64/mingw64/bin/g++.exe",
    "C:/ProgramData/mingw64/mingw64/bin/g++.exe",
    "/usr/bin/g++",
    "/usr/local/bin/g++",
)


def find_compiler(names=("g++",)):
    """Return a usable compiler path, or None when the machine has none."""
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    for candidate in _INSTALLED:
        if Path(candidate).is_file():
            directory = str(Path(candidate).parent)
            if directory.lower() not in os.environ.get("PATH", "").lower():
                os.environ["PATH"] = directory + os.pathsep + os.environ.get("PATH", "")
            return candidate
    return None
