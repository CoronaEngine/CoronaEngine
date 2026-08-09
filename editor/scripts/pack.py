"""Compatibility executable for the historical editor pack command."""

from pathlib import Path
import sys


_REPO_ROOT = Path(__file__).resolve().parents[2]
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

from editor.scripts.compat.legacy_pack import *  # noqa: F401,F403,E402


if __name__ == "__main__":
    main()
