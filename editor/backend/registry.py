"""Compatibility import for the canonical runtime service registry.

New host code should import :mod:`runtime.registry`. This path remains for
older embedded hosts and external editor integrations during migration.
"""

from runtime.registry import *  # noqa: F401,F403
