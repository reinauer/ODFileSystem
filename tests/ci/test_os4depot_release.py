#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Check publication inputs and transfer ordering without contacting OS4Depot."""

import contextlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "os4depot_upload", ROOT / ".github/actions/os4depot-release/upload.py"
)
UPLOAD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(UPLOAD)


class UploadTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.archive = Path(self.directory.name) / "OS4 package.lha"
        self.archive.write_bytes(b"existing release archive\x00\xff")
        self.env = {
            "OS4DEPOT_ARCHIVE": str(self.archive),
            "OS4DEPOT_FILENAME": "odfilesystem.lha",
            "OS4DEPOT_README": str(ROOT / "docs/ODFileSystem_OS4.os4depot"),
            "OS4DEPOT_VERSION": "v0.8.0",
            "OS4DEPOT_PASSPHRASE": r"test & / \ $ ` [VERSION]",
        }
        self.environment = patch.dict(os.environ, self.env)
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def test_upload_preserves_archive_and_sends_readme_last(self):
        transfers = []

        def transfer(command, check):
            self.assertTrue(check)
            source = Path(command[command.index("--upload-file") + 1])
            transfers.append((source, source.read_bytes(), command[-1]))

        output = io.StringIO()
        with patch.object(UPLOAD.subprocess, "run", side_effect=transfer), \
                contextlib.redirect_stdout(output):
            UPLOAD.main()

        self.assertEqual(len(transfers), 2)
        self.assertEqual(transfers[0][1], self.archive.read_bytes())
        self.assertEqual(transfers[0][2], "ftp://os4depot.net/upload/odfilesystem.lha")
        self.assertEqual(transfers[1][2], "ftp://os4depot.net/upload/odfilesystem_lha.readme")
        readme = transfers[1][1].decode("utf-8")
        self.assertIn("\nversion: 0.8.0\n", readme)
        self.assertIn("\nreplaces: driver/filesystem/odfilesystem.lha\n", readme)
        self.assertIn("\npassphrase: " + self.env["OS4DEPOT_PASSPHRASE"] + "\n", readme)
        self.assertIn("\nhend:\nODFileSystem", readme)
        self.assertNotIn(self.env["OS4DEPOT_PASSPHRASE"], output.getvalue())
        self.assertFalse(transfers[1][0].exists())
        self.assertTrue(self.archive.exists())

    def test_invalid_inputs_never_upload(self):
        cases = [
            ("OS4DEPOT_PASSPHRASE", ""),
            ("OS4DEPOT_PASSPHRASE", "x" * 41),
            ("OS4DEPOT_PASSPHRASE", "one\ntwo"),
            ("OS4DEPOT_FILENAME", "../unexpected.lha"),
            ("OS4DEPOT_VERSION", "v12345678901"),
            ("OS4DEPOT_VERSION", "v0.8\nname: injected"),
            ("OS4DEPOT_ARCHIVE", str(self.archive) + ".missing"),
        ]
        for key, value in cases:
            with self.subTest(key=key, value=value), \
                    patch.dict(os.environ, {key: value}), \
                    patch.object(UPLOAD.subprocess, "run") as run:
                with self.assertRaises(ValueError):
                    UPLOAD.main()
                run.assert_not_called()

    def test_transfer_failure_stops_submission_and_cleans_readme(self):
        for fail_at in (1, 2):
            transfers = []
            temporary_paths = []

            def transfer(command, check):
                source = Path(command[command.index("--upload-file") + 1])
                transfers.append(source)
                if len(transfers) == fail_at:
                    raise subprocess.CalledProcessError(1, command)

            original_write = Path.write_text

            def write_text(path, *args, **kwargs):
                temporary_paths.append(path)
                return original_write(path, *args, **kwargs)

            with self.subTest(fail_at=fail_at), \
                    patch.object(UPLOAD.subprocess, "run", side_effect=transfer), \
                    patch.object(Path, "write_text", write_text):
                with self.assertRaises(subprocess.CalledProcessError):
                    UPLOAD.main()
            self.assertEqual(len(transfers), fail_at)
            self.assertEqual(len(temporary_paths), 1)
            self.assertFalse(temporary_paths[0].exists())


if __name__ == "__main__":
    unittest.main()
