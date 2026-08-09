"""Compatibility import path for the canonical editor API adapter."""

import importlib
import sys


_canonical = importlib.import_module("api.editor_api")
sys.modules[__name__] = _canonical
