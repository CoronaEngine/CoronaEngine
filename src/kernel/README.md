# Engine kernel compatibility

CoronaEngine owns the `Corona::Kernel` public interfaces. The Horizon
`refactor_dsl` branch moved the logger, object pool, and stack-trace APIs out
of their previous `corona/kernel/...` include paths.

`include/corona/kernel/core/i_logger.h` retains `CoronaLogger`, `LogLevel`, and
the `CFW_LOG_*`, `PY_LOG_*`, and `VUE_LOG_*` macros.
`CoronaLogger` aliases an engine-owned implementation type to avoid duplicate
symbols with Horizon's internal compatibility class. It uses Horizon Core's
shared Quill logger through the `corona::engine::logging` target, preserving
formatting, source locations, level filtering, and nonfatal error logging. Call `CoronaLogger::initialize()` before
other Horizon logging to configure console output and a timestamped file under
`logs/`. Horizon's first successful logging initialization owns the sinks.
The implementation in `core/logger.cpp` keeps Core's legacy macros out of
consumer translation units, which also include the older rendering interfaces.
`cmake/corona_horizon_workspace.cmake` retains Core as a link-only dependency
of Horizon so its generic `core/...` search paths do not shadow Vision headers.
Code that uses the new Core API directly must link `horizon-core` explicitly.

`include/corona/kernel/utils/storage.h` and `stack_trace.h` retain the original
implementations from Horizon commit `5b4d13fad380`. They now belong to
CoronaEngine; this relocation preserves the object pool and lock diagnostics.

`HorizonKernelCompatTests` exercises the real logging backend, file output,
level mapping, filtered arguments, nonfatal errors, and storage growth and
read/write handles. It also compiles storage with `CFW_ENABLE_LOCK_TIMEOUT=1`
to cover its optional stack-trace dependency. The existing geometry and optics
storage snapshot tests cover the engine consumers.

## Running the compatibility checks on Windows

From the repository root, activate the generated Conan run environment before
running CTest so dependency DLLs are discoverable. For the CLion single-config
build used for this migration, run in cmd.exe:

```bat
call build\conan\examples\relwithdebinfo\generators\conanrun.bat
set CORONA_RUN_GPU_SMOKE=1
ctest --test-dir cmake-build-relwithdebinfo -C RelWithDebInfo --output-on-failure
```

The Vision tests use their runtime target directory for both single-config and
multi-config generators. The interop lifetime test uses Horizon's real
`SubmitReceipt`; a zero serial denotes an empty submission.
