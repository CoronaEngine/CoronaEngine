"""Compatibility wrapper for the AITool media service helpers."""

from ..services.media_storage import (  # noqa: F401
    base64_to_image_file,
    upload_file_to_server,
)

__all__ = ["base64_to_image_file", "upload_file_to_server"]
