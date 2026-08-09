"""Compatibility import for the canonical FileManager plugin.

The service used to live under the historical ``backend`` package. Keep this
module for old hosts and generated imports; new registrations must use
``plugins.FileManager.compat.legacy_file_manager``.
"""

from plugins.FileManager.compat.legacy_file_manager import FileManager

__all__ = ["FileManager"]
