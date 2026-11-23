"""Loader for the native CoronaEngine module."""
from importlib import import_module
from typing import Optional


# A marker has to be a name the module actually binds, and one whose absence is reported.
# The loader used to recognise the module by `Scene`, which the object-model removal deleted:
# recognition then returned None for *every* optional binding (send_log, the service
# dispatcher registration, the project session), leaving the editor on "archive service
# initialising" while nothing raised. That `Scene` marker is gone for good; `send_log` is
# host support, so it survives engine generations that come and go above it.
LOADER_MARKERS = ('send_log',)


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
            # 用宿主必然绑定的入口认模块，而不是用某个可能被删掉的类
            if any(hasattr(mod, marker) for marker in LOADER_MARKERS):
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
