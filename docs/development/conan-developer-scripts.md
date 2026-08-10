# Python 开发工作流

CoronaEngine 的唯一开发入口是 `tools/dev.py`。开发工具环境由 Conda Forge 中的 Conda 环境管理；开发机只需要预装 Miniconda / Conda、CMake、Ninja 和 Visual Studio C++ 工具链，不需要单独安装 Python 或 Conan。

首次使用时创建环境：

```text
conda create --yes --name coronaengine-dev --override-channels --channel conda-forge "python>=3.13,<3.14" "conan>=2.28,<3"
```

默认配置为 `RelWithDebInfo`，默认构建目标为 `corona_engine`：

```text
conda run -n coronaengine-dev --no-capture-output python tools/dev.py status
conda run -n coronaengine-dev --no-capture-output python tools/dev.py install
conda run -n coronaengine-dev --no-capture-output python tools/dev.py configure
conda run -n coronaengine-dev --no-capture-output python tools/dev.py build
conda run -n coronaengine-dev --no-capture-output python tools/dev.py build-fast
conda run -n coronaengine-dev --no-capture-output python tools/dev.py rebuild
conda run -n coronaengine-dev --no-capture-output python tools/dev.py update
conda run -n coronaengine-dev --no-capture-output python tools/dev.py clean
```

可在命令后指定目标，并用 `--configuration Debug|Release|RelWithDebInfo|MinSizeRel` 选择配置。例如：

```text
conda run -n coronaengine-dev --no-capture-output python tools/dev.py build corona_engine --configuration RelWithDebInfo
```

`build` 会安装依赖、配置并编译；`build-fast` 只复用已配置的构建树；`rebuild` 只删除当前配置目录后重建；`clean` 删除仓库生成的构建/安装目录，但保留 `.workspace/Horizon`。

## Horizon 工作区

Horizon 不再是 Conan package 或 editable。首次 `install`、`configure`、`build` 或 IDE 配置时，脚本会按 `.workspace/horizon.lock.json` 克隆到 `.workspace/Horizon`，然后由 CoronaEngine 通过 `add_subdirectory()` 直接编译源码。

锁文件同时固定 Git URL、ref 和完整 commit。普通命令不会 fetch/pull/reset：

- HEAD 与锁定 commit 一致时允许本地 dirty 修改。
- HEAD 不一致时立即失败，脚本不会覆盖开发者工作。
- `update` 只在工作区干净时 fetch 指定 ref，更新锁文件并切换到新的完整 commit。

## IDE / CMake Presets

VS、VSCode CMake Tools 和 CLion 直接选择根目录 `CMakePresets.json` 中的 `relwithdebinfo`、`debug`、`release` 或 `minsizerel`。首次 CMake configure 会在 `project()` 前通过 Conda 自动完成依赖和 Horizon 工作区 bootstrap。

各配置目录为 `build/conan/<configuration>`。

## Conan cache

全局 Conan cache 维护入口是 `tools/conan_cache.py`：

```text
conda run -n coronaengine-dev --no-capture-output python tools/conan_cache.py list
conda run -n coronaengine-dev --no-capture-output python tools/conan_cache.py list slang --version 2026.10
conda run -n coronaengine-dev --no-capture-output python tools/conan_cache.py update slang --version 2026.10
conda run -n coronaengine-dev --no-capture-output python tools/conan_cache.py remove slang/2026.10 --dry-run
conda run -n coronaengine-dev --no-capture-output python tools/conan_cache.py remove slang/2026.10 --force
conda run -n coronaengine-dev --no-capture-output python tools/conan_cache.py clear --dry-run
```

`remove` 默认要求输入 `YES`；`--force` 跳过交互确认。`clear` 操作当前用户的整个 Conan cache，应先使用 `--dry-run`。
