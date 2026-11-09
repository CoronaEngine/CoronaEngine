"""Loader for the native CoronaEngine module."""
from importlib import import_module
from typing import Optional


def load_corona_engine() -> Optional[object]:
    candidates = [
        'CoronaEngine',
    ]
    for name in candidates:
        try:
            mod = import_module(name)

            # 如果模块有 CoronaEngine 类属性，返回该类
            if hasattr(mod, 'CoronaEngine'):
                return getattr(mod, 'CoronaEngine')

            # 否则，检查模块是否本身就是 CoronaEngine（原生C++模块的情况）
            # 如果模块有 Scene 属性，说明它是可用的引擎模块
            if hasattr(mod, 'Scene'):
                return mod
        except Exception:
            continue

    return None

corona_engine = None
def get_corona_engine() -> Optional[object]:
    global corona_engine
    if corona_engine is None:
        corona_engine = load_corona_engine()
    return corona_engine
