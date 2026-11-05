"""Offline package input validation; no credentials or network are used."""
import json
import re
import stat
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import release


def context(ref="refs/heads/feature/foo"):
    return dict(repository="owner/repo", ref=ref, sha="a" * 40, configuration="Release",
                run_id=123, run_number=2, run_attempt=1)


def make_packages(root, ctx):
    paths = []
    ref = ctx["ref"].split("/", 2)[2]
    if ctx["ref"].startswith("refs/tags/") and re.fullmatch(r"v?\d+(\.\d+){1,2}([-.][0-9A-Za-z.]+)?", ref):
        basename = "halionbridge-" + ref.removeprefix("v")
    else:
        basename = "halionbridge-" + (re.sub(r"-+", "-", re.sub(r"[^A-Za-z0-9._-]+", "-", ref)).strip("-") or "unknown") + "-" + ctx["sha"][:7]
    for platform in release.PLATFORMS:
        name = basename + "-" + platform + ".zip"
        archive = root / name
        with zipfile.ZipFile(archive, "w") as package:
            package.writestr("halionbridge.exe" if platform.startswith("windows") else "halionbridge", "binary")
            package.writestr("examples/test/halionbridge_build.lua", "return {}")
            for notice in release.REQUIRED_NOTICES:
                package.writestr(notice, "notice")
        release.write_manifest(archive, platform, ctx)
        paths.append(archive)
    return paths


class PackageValidationTests(unittest.TestCase):
    def test_complete_bundles_and_generated_checksum_are_repeatable(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            expected = make_packages(root, context())
            self.assertEqual(release.verify_assets(root, context()), expected)
            (root / "SHA256SUMS.txt").write_text("old generated file")
            self.assertEqual(release.verify_assets(root, context()), expected)
            (root / "SHA256SUMS.txt").write_text("")
            self.assertEqual(release.verify_assets(root, context()), expected)

    def test_unexpected_files_and_missing_or_wrong_sidecars_are_rejected(self):
        for problem in ("extra", "missing", "bad"):
            with self.subTest(problem=problem), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                make_packages(root, context())
                path = root / "windows-x64.sha256"
                if problem == "extra":
                    (root / "unexpected.txt").write_text("unexpected")
                elif problem == "missing":
                    path.unlink()
                else:
                    path.write_text("0" * 64 + "  wrong.zip\n")
                with self.assertRaises(ValueError):
                    release.verify_assets(root, context())

    def test_wrong_archive_basename_with_matching_hash_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archives = make_packages(root, context())
            archives[0].rename(root / "wrong.zip")
            path = root / "windows-x64.json"
            manifest = json.loads(path.read_text())
            manifest["archive"] = "wrong.zip"
            path.write_text(json.dumps(manifest))
            (root / "windows-x64.sha256").write_text(manifest["sha256"] + "  wrong.zip\n")
            with self.assertRaises(ValueError):
                release.verify_assets(root, context())

    def test_manifest_shape_and_boolean_run_identity_are_rejected(self):
        for content in ("[]", "null", "invalid", '{"run_id":123,"run_id":124}'):
            with self.subTest(content=content), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                make_packages(root, context())
                (root / "windows-x64.json").write_text(content)
                with self.assertRaises(ValueError):
                    release.verify_assets(root, context())
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            ctx = dict(context(), run_attempt=1)
            make_packages(root, ctx)
            path = root / "windows-x64.json"
            manifest = json.loads(path.read_text())
            manifest["run_attempt"] = True
            path.write_text(json.dumps(manifest))
            with self.assertRaises(ValueError):
                release.verify_assets(root, ctx)

    def test_empty_cli_and_symlink_zip_members_are_rejected(self):
        for mode in ("empty", "symlink"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                archive = Path(directory) / "package.zip"
                with zipfile.ZipFile(archive, "w") as package:
                    member = zipfile.ZipInfo("halionbridge.exe")
                    member.create_system = 3
                    member.external_attr = ((stat.S_IFLNK if mode == "symlink" else stat.S_IFREG) | 0o755) << 16
                    package.writestr(member, "target" if mode == "symlink" else "")
                    package.writestr("examples/test/halionbridge_build.lua", "return {}")
                    for notice in release.REQUIRED_NOTICES:
                        package.writestr(notice, "notice")
                with self.assertRaises(ValueError):
                    release.verify_zip(archive, "windows-x64")

    def test_source_names_follow_existing_cmake_and_optional_v_conventions(self):
        refs = ("refs/heads/feature/foo", "refs/heads/Feature/foo", "refs/heads/refs/tags/foo",
                "refs/heads/a--b", "refs/tags/0.5.0", "refs/tags/v0.5.0", "refs/tags/v0.5.0-rc.1", "refs/tags/custom/tag")
        for ref in refs:
            with self.subTest(ref=ref), tempfile.TemporaryDirectory() as directory:
                root, ctx = Path(directory), context(ref)
                archives = make_packages(root, ctx)
                self.assertEqual(release.verify_assets(root, ctx), archives)

    def test_symlinked_inputs_and_generated_checksum_are_rejected(self):
        for name in ("windows-x64.json", "windows-x64.sha256", "halionbridge-feature-foo-aaaaaaa-windows-x64.zip", "SHA256SUMS.txt"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                root = Path(directory) / "input"
                root.mkdir()
                make_packages(root, context())
                path = root / name
                if not path.exists():
                    path.write_text("generated")
                outside = Path(directory) / "original"
                path.rename(outside)
                try:
                    path.symlink_to(outside)
                except OSError as error:
                    self.skipTest("Filesystem does not permit symlink fixtures: " + str(error))
                with self.assertRaises(ValueError):
                    release.verify_assets(root, context())
