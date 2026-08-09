#!/usr/bin/env python3
"""
CabbageEditor 项目打包脚本

将项目打包为 tar.gz 格式，排除运行时生成内容、环境依赖、缓存等非必要文件。
默认包含 InnerAgentWorkflow 子仓库代码。

用法:
    python pack.py                     # 默认打包
    python pack.py --output my.tar.gz  # 指定输出文件名
    python pack.py --exclude-workflow  # 排除子仓库
    python pack.py --dry-run           # 仅预览文件列表
"""

from __future__ import annotations

import argparse
import fnmatch
import os
import tarfile
from datetime import datetime
from pathlib import Path


# ==============================================================================
# 排除规则配置
# ==============================================================================

# 排除的目录（相对于项目根目录）
EXCLUDE_DIRS = {
    # 版本控制
    ".git",
    # 运行时生成
    "autosave",
    "workflow_output",
    "tests/output",
    # 环境依赖
    ".venv",
    "venv",
    "Env",
    "Frontend/node_modules",
    "Frontend/dist",
    "Frontend/.vite",
    "Frontend/.cache",
    # 缓存
    "__pycache__",
    ".pytest_cache",
    ".mypy_cache",
    ".ruff_cache",
    ".eggs",
    "htmlcov",
    # 编辑器配置
    ".vscode",
    ".idea",
    # 备份目录
    # 当前目录布局使用小写 backend；旧的大写名称保留用于历史项目包。
    "backend_backup",
    "backend_legacy",
    "Backend_backup",
    "Backend_legacy",
    # 旧 Quasar 测试报告路径（当前 AITool/Quasar 子模块另行管理）
    "backend/Quasar/experiments/service_tests/reports",
    "Backend/Quasar/experiments/service_tests/reports",
}

# 排除的文件模式（fnmatch 模式）
EXCLUDE_FILE_PATTERNS = {
    # 日志和临时文件
    "*.log",
    "*.tmp",
    "*.swp",
    "*.bak",
    # Python 编译文件
    "*.pyc",
    "*.pyo",
    "*.pyd",
    "*.so",
    # 编辑器文件
    "*.code-workspace",
    ".DS_Store",
    "Thumbs.db",
    # 测试覆盖率
    ".coverage",
    "coverage.xml",
    # 其他
    "llms.txt",
    "package-lock.json",
    # 生成的代码
    "blockly_code*.py",
    "runScript.py",
}

# 排除的特定文件路径（相对于项目根目录）
EXCLUDE_FILES = {
    # 当前目录布局
    "backend/script/blockly_code.py",
    "backend/runScript.py",
    # 历史目录布局
    "Backend/script/blockly_code.py",
    "Backend/runScript.py",
}


def should_exclude(path: str, exclude_workflow: bool = False) -> bool:
    """
    判断路径是否应该被排除

    参数:
        path: 相对于项目根目录的路径
        exclude_workflow: 是否排除 InnerAgentWorkflow 子仓库

    返回:
        True 表示应该排除，False 表示应该包含
    """
    # 标准化路径
    path = path.replace(os.sep, "/")
    parts = path.split("/")

    # 检查是否排除子仓库
    if exclude_workflow and path.startswith("InnerAgentWorkflow"):
        return True

    # 检查目录排除规则
    for part in parts:
        if part in EXCLUDE_DIRS:
            return True

    # 检查完整路径是否匹配目录排除规则
    for exclude_dir in EXCLUDE_DIRS:
        if path.startswith(exclude_dir + "/") or path == exclude_dir:
            return True
        # 检查路径中是否包含排除目录
        if f"/{exclude_dir}/" in f"/{path}/":
            return True

    # 检查特定文件排除
    if path in EXCLUDE_FILES:
        return True

    # 检查文件名模式排除
    filename = parts[-1]
    for pattern in EXCLUDE_FILE_PATTERNS:
        if fnmatch.fnmatch(filename, pattern):
            return True

    return False


def get_files_to_pack(
    root_dir: Path,
    exclude_workflow: bool = False,
) -> list[tuple[Path, str]]:
    """
    获取需要打包的文件列表

    参数:
        root_dir: 项目根目录
        exclude_workflow: 是否排除 InnerAgentWorkflow 子仓库

    返回:
        列表，每项为 (绝对路径, 相对路径)
    """
    files_to_pack = []

    for dirpath, dirnames, filenames in os.walk(root_dir):
        rel_dir = os.path.relpath(dirpath, root_dir)
        if rel_dir == ".":
            rel_dir = ""

        # 过滤要遍历的子目录（原地修改 dirnames 可以阻止 os.walk 进入这些目录）
        dirnames[:] = [
            d
            for d in dirnames
            if not should_exclude(
                os.path.join(rel_dir, d) if rel_dir else d,
                exclude_workflow,
            )
        ]

        # 处理文件
        for filename in filenames:
            rel_path = os.path.join(rel_dir, filename) if rel_dir else filename
            if not should_exclude(rel_path, exclude_workflow):
                abs_path = Path(dirpath) / filename
                files_to_pack.append((abs_path, rel_path))

    return files_to_pack


def format_size(size_bytes: int) -> str:
    """格式化文件大小"""
    for unit in ["B", "KB", "MB", "GB"]:
        if size_bytes < 1024:
            return f"{size_bytes:.2f} {unit}"
        size_bytes /= 1024
    return f"{size_bytes:.2f} TB"


def create_archive(
    root_dir: Path,
    output_path: Path,
    exclude_workflow: bool = False,
    dry_run: bool = False,
) -> tuple[int, int]:
    """
    创建 tar.gz 压缩包

    参数:
        root_dir: 项目根目录
        output_path: 输出文件路径
        exclude_workflow: 是否排除 InnerAgentWorkflow 子仓库
        dry_run: 是否仅预览

    返回:
        (文件数量, 压缩包大小)
    """
    files = get_files_to_pack(root_dir, exclude_workflow)

    if dry_run:
        print("\n📋 将要打包的文件列表：")
        print("=" * 60)
        for _, rel_path in sorted(files, key=lambda x: x[1]):
            print(f"  {rel_path}")
        print("=" * 60)
        print(f"\n📊 总计: {len(files)} 个文件")
        return len(files), 0

    # 获取项目名称作为压缩包内的根目录
    archive_root = root_dir.name

    print(f"\n📦 正在创建压缩包: {output_path}")
    print(f"   项目目录: {root_dir}")
    print(f"   包含子仓库: {'否' if exclude_workflow else '是'}")
    print()

    with tarfile.open(output_path, "w:gz", compresslevel=9) as tar:
        for i, (abs_path, rel_path) in enumerate(files, 1):
            # 在压缩包内添加项目根目录名作为前缀
            arcname = f"{archive_root}/{rel_path}"
            tar.add(abs_path, arcname=arcname)

            # 进度显示
            if i % 100 == 0 or i == len(files):
                print(f"\r   进度: {i}/{len(files)} 文件", end="", flush=True)

    print()  # 换行

    archive_size = output_path.stat().st_size
    return len(files), archive_size


def main():
    """主函数"""
    parser = argparse.ArgumentParser(
        description="将 CabbageEditor 项目打包为 tar.gz 格式",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  python pack.py                        # 默认打包
  python pack.py -o release.tar.gz      # 指定输出文件名
  python pack.py --exclude-workflow     # 排除 InnerAgentWorkflow 子仓库
  python pack.py --dry-run              # 预览将要打包的文件
        """,
    )

    parser.add_argument(
        "-o",
        "--output",
        type=str,
        default=None,
        help="输出文件名（默认: CabbageEditor-YYYYMMDD.tar.gz）",
    )

    parser.add_argument(
        "--exclude-workflow",
        action="store_true",
        help="排除 InnerAgentWorkflow 子仓库",
    )

    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="仅预览将要打包的文件列表，不实际创建压缩包",
    )

    args = parser.parse_args()

    # 确定项目根目录（脚本所在目录）
    root_dir = Path(__file__).parent.parent.resolve()

    # 确定输出文件名
    if args.output:
        output_path = Path(args.output)
        if not output_path.is_absolute():
            output_path = root_dir / output_path
    else:
        date_str = datetime.now().strftime("%Y%m%d")
        output_path = root_dir / f"CabbageEditor-{date_str}.tar.gz"

    print("=" * 60)
    print("  CabbageEditor 项目打包工具")
    print("=" * 60)

    # 创建压缩包
    file_count, archive_size = create_archive(
        root_dir=root_dir,
        output_path=output_path,
        exclude_workflow=args.exclude_workflow,
        dry_run=args.dry_run,
    )

    # 输出结果
    if not args.dry_run:
        print("\n✅ 打包完成！")
        print(f"   输出文件: {output_path}")
        print(f"   文件数量: {file_count}")
        print(f"   压缩大小: {format_size(archive_size)}")
    else:
        print("\n💡 使用 --dry-run 模式，未创建实际文件")
        print("   如需打包，请运行: python pack.py")

    print()


if __name__ == "__main__":
    main()
