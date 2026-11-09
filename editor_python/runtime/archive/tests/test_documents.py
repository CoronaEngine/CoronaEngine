import hashlib
import json
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

from runtime.archive.documents import read_document, commit_document
from runtime.archive.assets import asset_operation
from runtime.archive.editor_settings import editor_settings
from runtime.archive.project_files import file_manager


class ArchiveOwnershipTests(unittest.TestCase):
    def setUp(self):
        self.temp = TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.scope = {"archive_session": "first", "project_path": str(self.root)}
        self.active = dict(self.scope)
        self.enterContext(patch("runtime.project_context.read_project_session", lambda: self.active))

    def test_document_conflicts_and_same_path_reopened(self):
        request = {**self.scope, "document": "notes/task.json"}
        version = read_document(request)["version"]
        with ThreadPoolExecutor(4) as pool:
            results = list(pool.map(lambda i: commit_document({**request, "version": version, "content": i}), range(4)))
        self.assertEqual(sum(r["success"] for r in results), 1)
        self.assertEqual(sum(r.get("error") == "DOCUMENT_CONFLICT" for r in results), 3)
        current = read_document(request)
        self.active["archive_session"] = "second"
        self.assertEqual(commit_document({**request, "version": current["version"], "content": "late"})["error"],
                         "PROJECT_CONTEXT_CHANGED")
        self.assertEqual(read_document({**request, **self.active}), current)

    def test_structured_document_revision_conflicts_without_returning_source_text(self):
        request = {**self.scope, "document": "notes/task.json", "revision_only": True}
        initial = read_document(request)
        self.assertNotIn("version", initial)
        self.assertEqual(initial["revision"], hashlib.sha256(b"").hexdigest())
        content = {"unicode": "中文\u0000", "array": [None, {}, [], 4294967297]}
        with ThreadPoolExecutor(4) as pool:
            results = list(pool.map(lambda _: commit_document({**request,
                "revision": initial["revision"], "content": content}), range(4)))
        self.assertEqual(sum(r["success"] for r in results), 1)
        self.assertEqual(sum(r.get("error") == "DOCUMENT_CONFLICT" for r in results), 3)
        current = read_document(request)
        self.assertNotIn("version", current)
        self.assertEqual(current["content"], content)
        raw = (self.root / request["document"]).read_bytes()
        self.assertEqual(current["revision"], hashlib.sha256(raw).hexdigest())
        self.assertEqual(read_document({**request, "revision_only": False})["version"], raw.decode("utf8"))
        self.assertEqual(commit_document({**request, "version": raw.decode("utf8"), "content": {}})["error"],
                         "DOCUMENT_CONFLICT")
        self.active["archive_session"] = "second"
        self.assertEqual(commit_document({**request, "revision": current["revision"], "content": {}})["error"],
                         "PROJECT_CONTEXT_CHANGED")

    def test_structured_document_corrupt_backup_and_run_fence(self):
        path = self.root / "broken.json"
        path.write_bytes(b"{broken")
        request = {**self.scope, "document": path.name, "revision_only": True}
        result = read_document(request)
        self.assertNotIn("version", result)
        self.assertTrue(result["corrupt"])
        self.assertEqual(result["revision"], hashlib.sha256(b"{broken").hexdigest())
        write = {**request, "revision": result["revision"], "content": {"fixed": True}}
        with patch("script_runtime.file_runner.FileScripts.has_restore_snapshot", return_value=True):
            self.assertEqual(commit_document(write)["error"], "RUN_ACTIVE")
        saved = commit_document(write)
        self.assertTrue(saved["success"])
        self.assertNotIn("version", saved)
        self.assertEqual(saved["revision"], read_document(request)["revision"])
        self.assertEqual(next(self.root.glob("*.corrupt.json")).read_bytes(), b"{broken")

    def test_document_corrupt_backup_and_path_boundary(self):
        path = self.root / "broken.json"
        path.write_bytes(b"{broken")
        request = {**self.scope, "document": path.name}
        result = read_document(request)
        self.assertTrue(result["corrupt"])
        self.assertTrue(commit_document({**request, "version": result["version"], "content": {}})["success"])
        self.assertEqual(next(self.root.glob("*.corrupt.json")).read_bytes(), b"{broken")
        self.assertEqual(read_document({**request, "document": "../escape.json"})["error"], "INVALID_PROJECT_DOCUMENT")

    def test_settings_and_actor_template_stay_in_the_requested_location(self):
        path = self.root / "CoronaEditor.ini"
        path.write_text("[General]\nversion=1.0\n[Other]\n; keep this comment\nkeep=yes\n", encoding="utf8")
        base = {"ini": str(path), "section": "History"}
        for name in ("A", "B", "A"):
            editor_settings({**base, "recent": name})
        self.assertEqual(json.loads(editor_settings({**base, "key": "recent_projects"})["value"]), ["A", "B"])
        self.assertIn("; keep this comment\nkeep=yes", path.read_text())
        self.assertEqual(editor_settings({"ini": str(path), "section": "General", "key": "last_project"})["value"], "A")
        req = {**self.scope, "op": "create_file", "args": ["", "Hero", "actor"]}
        self.assertEqual(file_manager(req)["path"], "Hero.actor")
        self.assertEqual(file_manager(req)["path"], "Hero_1.actor")
        self.active["archive_session"] = "B"
        with self.assertRaisesRegex(ValueError, "PROJECT_CONTEXT_CHANGED"):
            file_manager(req)

    def test_preview_asset_reuse_is_read_only_and_concurrent_imports_are_merged(self):
        root = self.root / "world"
        def ingest(name):
            source = self.root / name
            source.write_bytes(name.encode())
            return asset_operation({"root": str(root), "op": "import", "category": "Models", "source": str(source),
                                    "files": [{"source": str(source), "relative": name}]})
        with ThreadPoolExecutor(2) as pool:
            results = list(pool.map(ingest, ("a.ply", "b.ply")))
        route = results[0]["bundle"]["main"]
        index = root / "assets.manifest.json"
        self.assertEqual(len(json.loads(index.read_bytes())["bundles"]), 2)
        def state():
            return {p.relative_to(root): (p.stat().st_mtime_ns, hashlib.sha256(p.read_bytes()).digest())
                    for p in root.rglob("*") if p.is_file()}
        before = state()
        with patch("script_runtime.file_runner.FileScripts.has_restore_snapshot",return_value=True):
            self.assertEqual(asset_operation({"root": str(root), "op": "import", "source": str(root / route)})["bundle"]["main"], route)
            self.assertEqual(ingest("c.ply")["diagnostics"][0]["code"], "run_active")
        self.assertEqual(state(), before)
