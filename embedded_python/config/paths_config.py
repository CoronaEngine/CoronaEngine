"""
路径配置

只解析路径,不接触文件系统:这里的函数都不创建目录。启动期 import 不写盘是硬约束
——程序装在只读目录里也必须能启动。

只保留宿主持有的一个路径:编辑器(部署)根目录,``bootstrap`` 用它扩展 ``sys.path``。
其余路径都没有 Python 消费方了:项目内的 media/models/screenshots 解析已删除,前端
入口由 CEF 在 ``Utils::editor_directory()`` 下自己定位并加载,Python 不再保存副本。
"""

from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class PathsConfig:
    """路径配置"""

    repo_root: Path


def get_default_paths() -> PathsConfig:
    """获取默认路径配置(纯解析,不在磁盘上留下任何东西)"""
    # 从当前文件位置计算仓库(编辑器)根目录
    return PathsConfig(repo_root=Path(__file__).resolve().parents[1])


def get_repository_assets_dir() -> Path:
    """Return the repository-bundled asset root (read-only runtime fallback)."""
    return get_default_paths().repo_root.parent / "assets"
