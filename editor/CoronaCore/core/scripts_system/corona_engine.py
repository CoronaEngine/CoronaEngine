"""Compatibility alias for :mod:`script_runtime.engine.corona_engine`."""

import importlib
import sys


_canonical = importlib.import_module("script_runtime.engine.corona_engine")
sys.modules[__name__] = _canonical
