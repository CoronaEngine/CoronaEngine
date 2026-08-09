"""Compatibility import for the canonical FileManager plugin.

The service used to live under the historical ``backend`` package. Keep this
module for old hosts and generated imports; new registrations must use
``plugins.FileManager.main``.
"""

from plugins.FileManager.main import FileManager

__all__ = ["FileManager"]
