"""Compatibility import for the canonical Script Runtime adapter.

New generated scripts should import
``script_runtime.engine.corona_engine``.  This module remains for
existing Blockly/Scratch output and external projects that use the historical
path.
"""

from script_runtime.engine.corona_engine import *  # noqa: F401,F403
