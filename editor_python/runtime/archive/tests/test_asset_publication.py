"""Exercise atomic bundle publication under real Windows directory contention."""
from contextlib import contextmanager
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
from tempfile import TemporaryDirectory
from threading import Event, Thread
import time
import unittest
from unittest.mock import patch

from runtime.archive.assets import asset_operation


@unittest.skipUnless(os.name == "nt", "Windows directory sharing contract")
class AssetPublicationTests(unittest.TestCase):
    def setUp(self):
        temporary = TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve() / "world"
        self.root.mkdir()
        self.source = self.root.parent / "triangle.ply"
        self.source.write_bytes(b"model fixture")
        self.manifest = self.root / "assets.manifest.json"
        self.manifest.write_text(json.dumps({"format": "corona_scene_assets", "version": 1, "bundles": []}))
        self.before = self.manifest.read_bytes()
        self.errors = []
        self.attempts = 0
        self.first_failure = Event()
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                           ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        self.kernel.CreateFileW.restype = wintypes.HANDLE
        self.kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        self.rename = os.rename

    def observe_rename(self, source, destination, *args, **kwargs):
        self.attempts += 1
        try:
            return self.rename(source, destination, *args, **kwargs)
        except OSError as error:
            self.errors.append(error.winerror)
            self.first_failure.set()
            raise

    @contextmanager
    def locked_stage(self, *args, persistent=False, **kwargs):
        with TemporaryDirectory(*args, **kwargs) as stage:
            # A reader may open the staged resource without FILE_SHARE_DELETE.
            # File reads/writes remain possible, but renaming its parent fails.
            staged_file = Path(stage) / self.source.name
            staged_file.write_bytes(b"")
            handle = self.kernel.CreateFileW(str(staged_file), 0x80000000, 1 | 2, None, 3, 0x80, None)
            self.assertNotEqual(handle, ctypes.c_void_p(-1).value)
            worker = None
            if not persistent:
                def release_after_failure():
                    self.first_failure.wait(5)
                    self.kernel.CloseHandle(handle)
                worker = Thread(target=release_after_failure)
                worker.start()
            try:
                yield stage
            finally:
                if worker:
                    self.first_failure.set()
                    worker.join()
                else:
                    self.kernel.CloseHandle(handle)

    def publish(self):
        return asset_operation({"root": str(self.root), "op": "import", "source": str(self.source),
                                "category": "Models", "files": [{"source": str(self.source), "relative": self.source.name}]})

    def test_transient_directory_lock_publishes_one_complete_bundle(self):
        with patch("runtime.archive.assets.TemporaryDirectory", self.locked_stage), patch("os.rename", self.observe_rename):
            result = self.publish()
        self.assertTrue(self.errors, result)
        self.assertTrue(all(code in (5, 32) for code in self.errors), self.errors)
        self.assertEqual(result["diagnostics"], [])
        main = self.root / result["bundle"]["main"]
        self.assertEqual(main.read_bytes(), self.source.read_bytes())
        self.assertEqual(json.loads(self.manifest.read_bytes())["bundles"], [result["bundle"]])
        self.assertFalse(list(self.root.glob(".asset-stage-*")))

    def test_persistent_directory_lock_fails_without_publishing_or_changing_manifest(self):
        def persistent_stage(*args, **kwargs):
            return self.locked_stage(*args, persistent=True, **kwargs)
        start = time.monotonic()
        with patch("runtime.archive.assets.TemporaryDirectory", persistent_stage), patch("os.rename", self.observe_rename):
            result = self.publish()
        self.assertLess(time.monotonic() - start, 2, "persistent denial must have a bounded failure path")
        self.assertTrue(self.errors, result)
        self.assertTrue(result["diagnostics"])
        self.assertEqual(self.manifest.read_bytes(), self.before)
        self.assertFalse(list(self.root.glob(".asset-stage-*")))
        self.assertFalse(list((self.root / "Assets/Models").glob("*")))

    def test_unrelated_rename_failure_is_not_retried(self):
        with patch("os.rename", side_effect=FileNotFoundError("destination removed")) as rename:
            result = self.publish()
        rename.assert_called_once()
        self.assertTrue(result["diagnostics"])
        self.assertEqual(self.manifest.read_bytes(), self.before)
        self.assertFalse(list(self.root.glob(".asset-stage-*")))


if __name__ == "__main__":
    unittest.main()
