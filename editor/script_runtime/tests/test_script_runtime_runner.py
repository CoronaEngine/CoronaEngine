from pathlib import Path
import sys


def test_generated_script_runner_prefers_script_runtime_output(tmp_path):
    generated = tmp_path / "runtime" / "generated"
    generated.mkdir(parents=True)
    (generated / "blockly_code.py").write_text(
        "def run():\n    return 'first'\n", encoding="utf-8"
    )

    previous_modules = {
        name: module
        for name, module in sys.modules.items()
        if name.startswith("corona_generated_blockly")
    }
    for name in previous_modules:
        sys.modules.pop(name, None)

    try:
        from editor.script_runtime.runner import run_generated_script

        assert run_generated_script(tmp_path) == "first"
        (generated / "blockly_code.py").write_text(
            "def run():\n    return 'second-result'\n", encoding="utf-8"
        )
        assert run_generated_script(tmp_path) == "second-result"
    finally:
        for name in list(sys.modules):
            if name.startswith("corona_generated_blockly"):
                sys.modules.pop(name, None)
        sys.modules.update(previous_modules)


def test_generated_script_runner_ignores_removed_legacy_output(tmp_path):
    backend = tmp_path / "backend"
    script = backend / "script"
    script.mkdir(parents=True)
    (backend / "__init__.py").write_text("", encoding="utf-8")
    (script / "__init__.py").write_text("", encoding="utf-8")
    (script / "blockly_code.py").write_text(
        "def run():\n    return 'first'\n", encoding="utf-8"
    )
    run_script = backend / "runScript.py"
    run_script.write_text(
        "from backend.script import blockly_code\n\n"
        "def run():\n    return blockly_code.run()\n",
        encoding="utf-8",
    )

    previous_modules = {
        name: module
        for name, module in sys.modules.items()
        if name == "backend" or name.startswith("backend.")
    }
    for name in previous_modules:
        sys.modules.pop(name, None)

    try:
        from editor.script_runtime.runner import run_generated_script

        assert run_generated_script(tmp_path) is None
        (script / "blockly_code.py").write_text(
            "def run():\n    return 'second-result'\n", encoding="utf-8"
        )
        assert run_generated_script(tmp_path) is None
    finally:
        for name in list(sys.modules):
            if name == "backend" or name.startswith("backend."):
                sys.modules.pop(name, None)
        sys.modules.update(previous_modules)


def test_generated_script_runner_returns_none_when_output_is_missing(tmp_path):
    from editor.script_runtime.runner import run_generated_script

    assert run_generated_script(Path(tmp_path)) is None
