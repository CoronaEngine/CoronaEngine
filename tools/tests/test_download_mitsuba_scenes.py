"""Offline integration tests for the Windows Mitsuba scene downloader."""

from __future__ import annotations

import io
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from zipfile import ZipFile


SCRIPT = Path(__file__).resolve().parents[2] / "tools/download_mitsuba_scenes.ps1"
RESULT_PREFIX = "MITSUBA_TEST_RESULT="


def ps_quote(value: object) -> str:
    return "'" + str(value).replace("'", "''") + "'"


class ArchiveHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        self.server.requests.append((self.path, self.headers.get("Range")))
        if self.path == "/mitsuba/missing.zip":
            self.send_error(404)
            return

        payload = self.server.payload
        range_header = self.headers.get("Range")
        start = int(re.fullmatch(r"bytes=(\d+)-", range_header).group(1)) if range_header else 0
        self.send_response(206 if range_header else 200)
        self.send_header("Content-Type", "application/zip")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(len(payload) - start))
        if range_header:
            self.send_header("Content-Range", f"bytes {start}-{len(payload) - 1}/{len(payload)}")
        self.end_headers()

        if self.server.interrupt:
            self.wfile.write(payload[start:start + 1024])
            self.wfile.flush()
            self.connection.shutdown(socket.SHUT_WR)
            self.close_connection = True
        else:
            self.wfile.write(payload[start:])

    def log_message(self, format, *args):
        pass


@unittest.skipUnless(os.name == "nt", "requires Windows PowerShell and curl.exe")
class DownloadMitsubaScenesTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(SCRIPT.is_file(), "Mitsuba downloader has not been implemented")
        self.temporary = tempfile.TemporaryDirectory(prefix="mitsuba scenes ")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name) / "downloads"
        buffer = io.BytesIO()
        with ZipFile(buffer, "w") as archive:
            archive.writestr("scene.xml", b"<scene version='3.0.0'/>\n" * 1024)
        self.payload = buffer.getvalue()

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), ArchiveHandler)
        self.server.payload = self.payload
        self.server.requests = []
        self.server.interrupt = False
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.addCleanup(self.stop_server)
        self.base_uri = f"http://127.0.0.1:{self.server.server_port}"

    def stop_server(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)

    def powershell(self, body):
        command = (
            "$ErrorActionPreference = 'Stop'; try { "
            f". {ps_quote(SCRIPT)}; {body}; "
            f"Write-Output ('{RESULT_PREFIX}' + (ConvertTo-Json -InputObject $result -Compress)); "
            "exit 0 } catch { Write-Error $_ -ErrorAction Continue; exit 1 }"
        )
        return subprocess.run(
            ["powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", command],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=45,
            check=False,
        )

    def successful_result(self, process):
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        lines = [line for line in process.stdout.splitlines() if line.startswith(RESULT_PREFIX)]
        self.assertEqual(len(lines), 1, process.stdout)
        return json.loads(lines[0][len(RESULT_PREFIX):])

    def save_archive(self, name):
        return self.powershell(
            f"$result = Save-MitsubaArchive -Uri {ps_quote(self.base_uri + '/mitsuba/' + name)} "
            f"-DestinationDirectory {ps_quote(self.directory)}"
        )

    def test_parser_keeps_mitsuba_archives_in_page_order_without_duplicates(self):
        html = """
            <a href='mitsuba/relative_m3.zip'>Mitsuba 3</a>
            <a href='/mitsuba/legacy.zip'>Legacy Mitsuba</a>
            <a href="https://example.test/mitsuba/relative_m3.zip">Duplicate</a>
            <a href='https://example.test/mitsuba/absolute.zip'>Absolute</a>
            <a href='/tungsten/tungsten.zip'>Tungsten</a>
            <a href='/pbrt/pbrt.zip'>PBRT</a>
            <a href='https://github.com/example/scenes/archive/main.zip'>GitHub</a>
            <a href='https://other.test/mitsuba/offsite.zip'>Other host</a>
            <a href='/mitsuba/readme.txt'>Text</a>
            <a href='/mitsuba/nested/scene.zip'>Nested path</a>
            <a href='/mitsuba/../outside.zip'>Traversal</a>
        """
        process = self.powershell(
            f"$result = @(Get-MitsubaArchiveUrls -Html {ps_quote(html)} "
            "-PageUri 'https://example.test/scenes.html')"
        )
        self.assertEqual(self.successful_result(process), [
            "https://example.test/mitsuba/relative_m3.zip",
            "https://example.test/mitsuba/legacy.zip",
            "https://example.test/mitsuba/absolute.zip",
        ])
        self.assertEqual(self.server.requests, [])

    def test_partial_archive_resumes_exact_bytes_then_existing_final_skips_http(self):
        self.directory.mkdir()
        partial = self.directory / "resume.zip.part"
        partial.write_bytes(self.payload[:4096])

        self.assertEqual(self.successful_result(self.save_archive("resume.zip")), "downloaded")
        self.assertEqual((self.directory / "resume.zip").read_bytes(), self.payload)
        self.assertFalse(partial.exists())
        self.assertEqual(self.server.requests, [("/mitsuba/resume.zip", "bytes=4096-")])

        self.assertEqual(self.successful_result(self.save_archive("resume.zip")), "skipped")
        self.assertEqual((self.directory / "resume.zip").read_bytes(), self.payload)
        self.assertEqual(len(self.server.requests), 1)

    def test_http_error_never_publishes_a_final_archive(self):
        process = self.save_archive("missing.zip")
        self.assertNotEqual(process.returncode, 0, process.stdout + process.stderr)
        self.assertTrue(self.directory.is_dir())
        self.assertFalse((self.directory / "missing.zip").exists())
        self.assertEqual(self.server.requests, [("/mitsuba/missing.zip", None)])

    def test_interrupted_transfer_keeps_partial_for_the_next_run(self):
        self.server.interrupt = True
        process = self.save_archive("interrupted.zip")
        self.assertNotEqual(process.returncode, 0, process.stdout + process.stderr)
        final = self.directory / "interrupted.zip"
        partial = self.directory / "interrupted.zip.part"
        self.assertFalse(final.exists())
        self.assertTrue(partial.is_file())
        self.assertGreater(partial.stat().st_size, 0)
        self.assertLess(partial.stat().st_size, len(self.payload))
        self.assertEqual(partial.read_bytes(), self.payload[:partial.stat().st_size])

        offset = partial.stat().st_size
        self.server.interrupt = False
        self.assertEqual(self.successful_result(self.save_archive("interrupted.zip")), "downloaded")
        self.assertEqual(final.read_bytes(), self.payload)
        self.assertFalse(partial.exists())
        self.assertEqual(self.server.requests[-1], ("/mitsuba/interrupted.zip", f"bytes={offset}-"))


if __name__ == "__main__":
    unittest.main()
