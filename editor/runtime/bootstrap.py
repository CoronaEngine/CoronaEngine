"""Canonical embedded Python runtime bootstrap.

The repository-level ``editor/main.py`` remains the host-facing import path,
but runtime initialization belongs to this module so it is not confused with
an editor plugin or a traditional backend service.
"""

import atexit
import logging
import sys
from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = EDITOR_ROOT.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.append(str(REPO_ROOT))

from config.app_config import get_app_config
from runtime.editor_host import CoronaEditor


app_config = get_app_config()
if str(app_config.paths.repo_root) not in sys.path:
    sys.path.append(str(app_config.paths.repo_root))

try:
    from runtime.logging import configure_logging

    configure_logging()
except Exception:
    import traceback as _tb

    _tb.print_exc()
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(name)s [%(filename)s:%(lineno)d] %(message)s",
        force=True,
    )


editor = CoronaEditor
editor.module_list["CoronaEditor"] = CoronaEditor
from runtime.registry import register_core_python_script_services

register_core_python_script_services()
editor.register_script_dispatcher()
atexit.register(editor.unregister_script_dispatcher)

try:
    from runtime.plugin_loader import reimport

    reimport()
except Exception:
    pass


def run():
    """Signal that the embedded Python runtime is ready for editor updates."""

    logging.info("Python script runtime initialized; C++ UI owns the Vue/CEF frontend tab.")
