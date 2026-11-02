These seven Horizon changes are build inputs owned by CoronaEngine. The five
production files implement fence-bound resource retention and reduce shader
argument packing overhead; the two tests cover the corresponding behavior.
`reblur-runtime.json` records the exact upstream commit and SHA256 values before
and after the patch (source line endings are normalized to LF).

`dev.py` prepares the pinned source workspaces and verifies/applies this patch
before dependency installation or a build. A fresh source checkout therefore
does not require another developer's uncommitted Horizon changes. Repeated
preparation does not rewrite already-patched files. Unknown edits to any of the
seven owned files stop preparation before any write; edits elsewhere, including
`src/tests/runtime/test_compute_math_parity.cpp`, are preserved.

This is the minimal delta required for the DSL optimization, not a full vendored
Horizon distribution. The existing bootstrap still downloads the pinned Horizon
base from its separate repository and installs external toolchain dependencies.
It does not establish an offline or zero-external-source build.

Validation uses temporary local Git repositories; no CMake, GPU, or network is
required. Use the project's Python 3.13 environment:

```text
conda run -n coronaengine-dev python -m unittest build_tools.tests.test_horizon_runtime_patch build_tools.tests.test_dev_targets build_tools.tests.test_git_workspace -v
```

Changing the Horizon pin requires regenerating/reviewing the patch and its
manifest. Run preparation after resolving edits to owned files; the helper
never resets or stashes a developer's work.
