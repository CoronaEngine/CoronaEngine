"""The packaged runner waits only for explicit pre-commit service readiness."""
import ast
import json
from pathlib import Path
from types import SimpleNamespace


def load_open():
    path = Path(__file__).with_name('web_packaged_ui_smoke.py')
    tree = ast.parse(path.read_text(encoding='utf-8'))
    functions = [node for node in tree.body if isinstance(node, ast.FunctionDef)
                 and node.name == 'open_initial_test_world']
    assert functions, 'packaged startup must handle explicit archive service readiness'
    attempts = []

    def wait(read, predicate, label):
        for _ in range(5):
            result = read()
            attempts.append(result)
            if predicate(result):
                return result
        raise AssertionError(label)

    namespace = {'json': json, 'web_environment_smoke': SimpleNamespace(wait=wait)}
    exec(compile(ast.Module(body=functions, type_ignores=[]), str(path), 'exec'), namespace)
    return namespace['open_initial_test_world'], attempts


def test_startup_waits_for_both_precommit_initialization_statuses():
    open_world, attempts = load_open()
    replies = iter([{'ok': False, 'status': 'service_initializing'},
                    {'ok': False, 'status': 'initializing'},
                    {'ok': True, 'status': 'opened', 'path': 'fixture'}])
    calls = []
    def evaluate(expression):
        calls.append(expression)
        return next(replies)
    result = open_world(SimpleNamespace(evaluate=evaluate), Path('fixture'))
    assert result['ok'] and len(attempts) == 3
    assert len(set(calls)) == 1, 'readiness must use the same fixture and native contract'


def test_startup_does_not_retry_success_or_real_open_failures():
    for result in [{'ok': True, 'status': 'opened'},
                   {'ok': False, 'status': 'invalid_archive'},
                   {'ok': False, 'status': 'archive_service_error'},
                   {'ok': False, 'status': 'script_cleanup_failed'}]:
        open_world, attempts = load_open()
        assert open_world(SimpleNamespace(evaluate=lambda _: result), Path('fixture')) is result
        assert len(attempts) == 1
