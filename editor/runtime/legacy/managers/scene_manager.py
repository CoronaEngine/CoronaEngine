"""
Scene Manager - Data-Oriented Programming (DOP) 风格
数据和操作分离，使用纯函数管理 Scene 资源
"""
from __future__ import annotations
from typing import Optional, List, Dict
from runtime.legacy.entities.scene import Scene

# ============================================================================
# 数据存储：模块级字典
# ============================================================================
_scenes: Dict[str, Scene] = {}


# ============================================================================
# 查询操作：纯函数
# ============================================================================
def get(route: str) -> Optional[Scene]:
    """获取指定名称的 Scene"""
    return _scenes.get(route)


def has(route: str) -> bool:
    """检查 Scene 是否存在"""
    return route in _scenes


def list_all() -> List[str]:
    """列出所有 Scene 名称"""
    return list(_scenes.keys())


def count() -> int:
    """获取 Scene 总数"""
    return len(_scenes)


# ============================================================================
# 创建操作：修改数据
# ============================================================================
def create(route: str) -> Scene:
    """创建新的 Scene"""
    if route in _scenes:
        # 如果已存在，直接返回（兼容旧逻辑）
        return _scenes[route]
    scene = Scene(route)
    scene.ensure_default_camera()
    _scenes[route] = scene
    return scene


def register(route: str, scene: Scene) -> None:
    """注册已存在的 Scene"""
    if route in _scenes:
        raise ValueError(f"Scene '{route}' already registered")
    _scenes[route] = scene


def get_or_create(route: str) -> Scene:
    """获取或创建 Scene（推荐）"""
    existing = get(route)
    if existing is not None:
        existing.ensure_default_camera()
        return existing
    return create(route)


# ============================================================================
# 删除操作：修改数据
# ============================================================================
def remove(route: str) -> bool:
    """删除指定名称的 Scene"""
    if route in _scenes:
        del _scenes[route]
        return True
    return False


def clear() -> None:
    """清空所有 Scene"""
    _scenes.clear()


# ============================================================================
# 批量操作
# ============================================================================
def create_batch(scene_names: List[str]) -> List[Scene]:
    """批量创建 Scene"""
    results = []
    for route in scene_names:
        scene = get_or_create(route)
        results.append(scene)
    return results


def remove_batch(routes: List[str]) -> int:
    """批量删除 Scene，返回删除的数量"""
    count = 0
    for route in routes:
        if remove(route):
            count += 1
    return count


# ============================================================================
# 全局 Actor 查询（跨所有 Scene）
# ============================================================================
def find_actor(name: str):
    """跨所有 Scene 按名称查找 Actor"""
    for scene in _scenes.values():
        actor = scene.find_actor(name)
        if actor is not None:
            return actor
    return None


def find_actor_by_route(route: str):
    """跨所有 Scene 按文件路径查找 Actor"""
    for scene in _scenes.values():
        actor = scene.find_actor_by_route(route)
        if actor is not None:
            return actor
    return None


# ============================================================================
# 调试与监控
# ============================================================================
def get_all() -> Dict[str, Scene]:
    """获取所有 Scene（用于调试）"""
    return _scenes.copy()


def print_state() -> None:
    """打印当前状态（用于调试）"""
    print(f"[SceneManager] Total: {count()}")
    for name in list_all():
        scene = get(name)
        print(f"  - {name}: {scene}")
