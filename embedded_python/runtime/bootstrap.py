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
from runtime import host_loop
from runtime.editor_host import CoronaEditor
from runtime.native_engine import get_corona_engine


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
CoronaEngine = get_corona_engine()
editor.register_script_dispatcher()
atexit.register(editor.unregister_script_dispatcher)

try:
    from runtime.plugin_loader import reimport

    reimport()
except Exception:
    pass


def run():
    """Initialize and start the editor runtime; returns immediately."""

    editor.initialize_runtime()
    editor.start_runtime()
    logging.getLogger(__name__).info("event=runtime.started")


def loop():
    """Own the loop until CoronaEngine.should_stop(); returns the shutdown snapshot."""

    if CoronaEngine is None:
        raise RuntimeError("CoronaEngine module is unavailable")
    return host_loop.run_loop(
        CoronaEngine, editor,
        dispatch=editor.dispatch_script_values_from_cpp)
