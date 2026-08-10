"""EngineWriteGate：引擎写入统一收口（突击方案 §2.4gate / 任务 D）。

定位（必须前置认清）：
- `_engine_lock` 是**状态一致性保护**，不是截图死锁解药。
  它解决：Agent import 与用户 move 同时发生 / 用户 delete 与 settlement 同时发生 /
  VLM screenshot 与 import 同时发生 —— 这些并发写入的交错竞态。
- 它**不能**解决 C++ 帧末队列内部的 screenshot 死锁；那继续靠 timeout + skip + 引擎修。

铁律（突击方案 §10）：所有引擎写入口都必须经过本 gate，禁止新增绕过的
import/remove/move 路径。生成可以并行、规划可以异步，但**引擎写入必须串行**。
"""
from __future__ import annotations

import logging
import threading
from typing import Any, Callable

logger = logging.getLogger(__name__)

# 进程级单锁：所有引擎写入串行经此。RLock 允许同线程重入（gate 内部调 gate）。
_engine_lock = threading.RLock()


def get_engine_lock() -> "threading.RLock":
    """暴露同一把锁，供必须手动加锁的少数路径复用（不要新建第二把锁）。"""
    return _engine_lock


class EngineWriteGate:
    """引擎写入串行收口。

    用法：所有 import/remove/transform/screenshot/settle 都经过这里。
    每个方法只是「拿锁 → 调底层 → 放锁」，不持有引擎状态——状态的唯一事实源是
    SceneLayout（突击方案 §1：不新建平行事实源）。
    """

    def __init__(self) -> None:
        self._lock = _engine_lock

    # ── 通用入口 ─────────────────────────────────────────────
    def run(self, fn: Callable[..., Any], *args, **kwargs) -> Any:
        """串行执行任意引擎写入函数。新增写入口一律走这里。"""
        with self._lock:
            return fn(*args, **kwargs)

    # ── 工具调用入口（StructuredTool.invoke 收口）──────────────
    def invoke_tool(self, tool: Any, payload: dict) -> Any:
        """串行调用一个引擎工具（import_model / remove_model / set_actor_transform 等）。

        tool 为 None 时返回 None（调用方按未注册处理），不抛异常。
        """
        if tool is None:
            logger.warning(
                "[EngineWriteGate] invoke_tool: tool 未注册，跳过 actor_name=%s",
                str((payload or {}).get("actor_name") or "")[:80],
            )
            return None
        with self._lock:
            return tool.invoke(payload)

    # ── 语义化白名单入口 ─────────────────────────────────────
    def import_model(self, tool: Any, payload: dict) -> Any:
        """导入模型。payload 至少包含 model_path/actor_name。"""
        return self.invoke_tool(tool, payload)

    def remove_actor(self, tool: Any, payload: dict) -> Any:
        """删除/移除 actor。payload 由底层 remove_model 工具定义。"""
        return self.invoke_tool(tool, payload)

    def set_transform(self, tool: Any, payload: dict) -> Any:
        """移动/旋转/缩放 actor。payload 由 set_actor_transform 工具定义。"""
        return self.invoke_tool(tool, payload)

    def set_material(self, tool: Any, payload: dict) -> Any:
        """修改材质/颜色。payload 由材质工具定义。"""
        return self.invoke_tool(tool, payload)

    def settle(self, fn: Callable[..., Any], *args, **kwargs) -> Any:
        """执行 settlement/re-layout 类写操作。"""
        with self._lock:
            return fn(*args, **kwargs)

    # ── 截图（特殊：锁只保一致性，死锁靠 timeout/skip）────────
    def screenshot(self, fn: Callable[..., Any], *args, **kwargs) -> Any:
        """串行执行截图。

        ⚠️ 锁仅防 screenshot 与 import/remove 的写入交错，**不防 C++ 帧末渲染同步
        死锁**。调用方必须自带 timeout + skip 兜底（VLM 截图是独立于物理的第二个
        卡死源，突击方案 §1 ⟦RISK:vlm-screenshot-deadlock⟧）。
        """
        with self._lock:
            return fn(*args, **kwargs)


# 进程级单例（按需取用；测试可各自 new 一个，锁仍是同一把）
_default_gate: EngineWriteGate | None = None


def get_engine_write_gate() -> EngineWriteGate:
    """取进程级默认 gate 单例。"""
    global _default_gate
    if _default_gate is None:
        _default_gate = EngineWriteGate()
    return _default_gate


__all__ = ["EngineWriteGate", "get_engine_write_gate", "get_engine_lock"]
