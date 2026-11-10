"""Exercise the pixel placement used by the display compositor, without a GPU."""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from toolchain import find_compiler  # noqa: E402
import shutil
import subprocess
import tempfile

import pytest

ROOT = Path(__file__).resolve().parents[3]


def test_fixed_image_is_maximally_fitted_without_stretching():
    compiler = find_compiler()
    if not compiler:
        pytest.skip("Standalone display placement check requires g++")
    code = r'''
#include "src/systems/display/aspect_fit.h"
#include <cassert>
using Corona::Systems::DisplayDetail::fit_image;
int main() {
    auto same=fit_image({0,0,1920,1080},1920,1080);
    assert(same.x==0 && same.y==0 && same.width==1920 && same.height==1080);
    auto square=fit_image({120,80,1024,1024},1920,1080);
    assert(square.x==120 && square.y==304 && square.width==1024 && square.height==576);
    auto wide=fit_image({0,0,2560,1080},1920,1080);
    assert(wide.x==320 && wide.y==0 && wide.width==1920 && wide.height==1080);
    auto dpi=fit_image({0,0,3840,2160},1920,1080);
    assert(dpi.x==0 && dpi.y==0 && dpi.width==3840 && dpi.height==2160);
    auto odd=fit_image({10,20,1536,825},1920,1080);
    assert(odd.x==44 && odd.y==20 && odd.width==1467 && odd.height==825);
    auto portrait=fit_image({0,0,1920,1080},1080,1920);
    assert(portrait.x==656 && portrait.y==0 && portrait.width==608 && portrait.height==1080);
}
'''
    temporary_root=ROOT / ".workspace/tmp/viewport-aspect-tests"
    temporary_root.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(dir=temporary_root) as folder:
        source=Path(folder)/"aspect.cpp"
        executable=Path(folder)/"aspect.exe"
        source.write_text(code,encoding="utf-8")
        compiled=subprocess.run([compiler,"-std=c++17","-I",str(ROOT),str(source),"-o",str(executable)],capture_output=True,text=True)
        assert compiled.returncode==0,compiled.stderr
        checked=subprocess.run([str(executable)],capture_output=True,text=True)
        assert checked.returncode==0,checked.stderr
