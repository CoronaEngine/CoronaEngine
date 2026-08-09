"""Compatibility alias for :mod:`script_runtime.engine.scripts_manager`."""

import importlib
import sys


_canonical = importlib.import_module("script_runtime.engine.scripts_manager")
sys.modules[__name__] = _canonical
