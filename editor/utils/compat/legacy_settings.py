"""Compatibility import for the canonical editor configuration settings.

New code should import :mod:`config.settings`. This historical path remains
for external plugins and older generated scripts during migration.
"""

from config.settings import *  # noqa: F401,F403
