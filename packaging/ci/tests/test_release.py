import json
import os
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import release
from release_backend import Backend


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        token = patch.dict(os.environ, GH_TOKEN="offline-test-token")
        token.start()
        self.addCleanup(token.stop)

    def test_hashing_supports_python_310_without_file_digest(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "input.bin"
            with patch.object(release.hashlib, "file_digest", create=True,
                              side_effect=AssertionError("file_digest requires Python 3.11")):
                for content in (b"", b"abc", b"\x00\xff\r\n" * 300000):
                    path.write_bytes(content)
                    self.assertEqual(release.digest(path), release.hashlib.sha256(content).hexdigest())

    def context(self, ref="refs/heads/main", run=2):
        return dict(repository="owner/repo", ref=ref, sha="a" * 40,
                    run_id=123, run_number=run, run_attempt=1, configuration="Release")

    def test_branch_targets_are_stable_and_collision_resistant(self):
        self.assertEqual(release.target(self.context("refs/heads/feature/foo")), "snapshot/feature/foo")
        self.assertEqual(release.target(self.context()), release.target(self.context(run=3)))
        self.assertNotEqual(release.target(self.context("refs/heads/feature/foo")),
                            release.target(self.context("refs/heads/feature-foo")))
        self.assertNotEqual(release.target(self.context("refs/heads/main")),
                            release.target(self.context("refs/heads/Main")))

    def test_tag_and_same_version_branch_are_independent(self):
        self.assertEqual(release.target(self.context("refs/tags/v1.2.3")), "v1.2.3")
        self.assertNotEqual(release.target(self.context("refs/heads/v1.2.3")), "v1.2.3")

    def test_only_owned_draft_can_be_replaced(self):
        old = dict(draft=True, body=release.notes(self.context(run=1)))
        release.check_existing(old, self.context())
        for bad in [dict(old, draft=False), dict(old, body="manual draft"),
                    dict(old, body=release.notes(self.context("refs/heads/other"))),
                    dict(old, body=release.notes(self.context(run=3)))]:
            with self.assertRaises(ValueError):
                release.check_existing(bad, self.context())

    def test_debug_never_produces_release_target(self):
        with self.assertRaises(ValueError):
            release.target(dict(self.context(), configuration="Debug"))

    def test_existing_published_tag_is_preserved_with_notes_and_assets(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            ctx = self.context("refs/tags/0.3.1")
            for platform in release.PLATFORMS:
                (root / (platform + ".json")).write_text("{}")
            backend = Backend(ctx)
            backend.current.update(draft=False, body="handwritten release notes",
                                   html_url="https://github.com/owner/repo/releases/tag/0.3.1")
            before = json.dumps([backend.current, backend.assets], sort_keys=True)
            with patch.dict(os.environ, GITHUB_EVENT_NAME="workflow_dispatch", GH_TOKEN="offline-test-token",
                            GITHUB_WORKFLOW="00 Full - All Platforms"), \
                 patch.object(release, "verify_assets", return_value=[]), \
                 patch.object(release, "api", side_effect=backend.api):
                result = release.draft(root, ctx)
            self.assertEqual(result, dict(action="preserved", url=backend.current["html_url"]))
            self.assertEqual(before, json.dumps([backend.current, backend.assets], sort_keys=True))
            self.assertEqual(backend.mutations, [])

    def test_verifies_complete_set_and_rejects_tampering(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for platform in release.PLATFORMS:
                package = root / ("halionbridge-main-aaaaaaa-" + platform + ".zip")
                with zipfile.ZipFile(package, "w") as archive:
                    archive.writestr("halionbridge.exe" if platform.startswith("windows") else "halionbridge", "binary")
                    archive.writestr("examples/test/halionbridge_build.lua", "return {}")
                    for name in release.REQUIRED_NOTICES:
                        archive.writestr(name, "notice")
                release.write_manifest(package, platform, self.context())
            self.assertEqual(len(release.verify_assets(root, self.context())), 3)
            for key, value in (("sha", "b" * 40), ("run_attempt", 2), ("configuration", "Debug")):
                with self.subTest(key=key), self.assertRaises(ValueError):
                    release.verify_assets(root, dict(self.context(), **{key: value}))
            (root / "halionbridge-main-aaaaaaa-windows-x64.zip").write_bytes(b"tampered")
            with self.assertRaises(ValueError):
                release.verify_assets(root, self.context())

    def test_partial_set_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError):
                release.verify_assets(Path(directory), self.context())

    def test_packaging_supports_source_with_and_without_dxsyx(self):
        for with_dxsyx in (False, True):
            with self.subTest(with_dxsyx=with_dxsyx), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                for origin in release.NOTICES.values():
                    if origin.startswith("forks/dxsyx/"):
                        continue
                    path = root / origin
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text("notice")
                example = "examples/test/halionbridge_build.lua"
                (root / example).parent.mkdir(parents=True)
                (root / example).write_text("return {}")
                binary = root / "halionbridge.exe"
                binary.write_bytes(b"fixture")
                if with_dxsyx:
                    (root / "forks/dxsyx").mkdir(parents=True)
                    for name in ("LICENSE.txt", "UPSTREAM.md"):
                        (root / "forks/dxsyx" / name).write_text("notice")
                with patch.object(release.subprocess, "check_output", return_value=(example + "\0").encode()):
                    archive = release.package(root, binary, root / "output", "halionbridge-test", "windows-x64", self.context())
                with zipfile.ZipFile(archive) as package:
                    self.assertEqual("licenses/dxsyx/LICENSE.txt" in package.namelist(), with_dxsyx)
                    self.assertEqual("licenses/dxsyx/UPSTREAM.md" in package.namelist(), with_dxsyx)
                if with_dxsyx:
                    (root / "forks/dxsyx/LICENSE.txt").unlink()
                    with patch.object(release.subprocess, "check_output", return_value=(example + "\0").encode()), self.assertRaises(ValueError):
                        release.package(root, binary, root / "output", "halionbridge-test", "windows-x64", self.context())

    def test_archives_reject_unexpected_and_unsafe_entries(self):
        with tempfile.TemporaryDirectory() as directory:
            for extra in ("../outside", "tests.exe", "examples/build/output.txt", "examples/output.vstpreset"):
                archive = Path(directory) / "package.zip"
                with zipfile.ZipFile(archive, "w") as package:
                    package.writestr("halionbridge.exe", "binary")
                    package.writestr("examples/test/halionbridge_build.lua", "return {}")
                    for name in release.REQUIRED_NOTICES:
                        package.writestr(name, "notice")
                    package.writestr(extra, "unexpected")
                with self.subTest(extra=extra), self.assertRaises(ValueError):
                    release.verify_zip(archive, "windows-x64")

    def test_only_all_platforms_release_dispatch_can_mutate(self):
        for event, workflow, configuration in (("push", "00 Full - All Platforms", "Release"),
                                                ("workflow_dispatch", "10 Build - Windows x64", "Release"),
                                                ("workflow_dispatch", "00 Full - All Platforms", "Debug")):
            with patch.dict(os.environ, GITHUB_EVENT_NAME=event, GITHUB_WORKFLOW=workflow), \
                 patch.object(release, "api") as api, self.assertRaises(ValueError):
                release.draft(Path("unused"), dict(self.context(), configuration=configuration))
            api.assert_not_called()


    def test_remote_tag_is_checked_without_mutation(self):
        ctx = self.context("refs/tags/v1.2.3")
        annotated = dict(ref=ctx["ref"], object=dict(type="tag", sha="b" * 40))
        with patch.object(release, "api", side_effect=[[annotated], {"object": {"type": "commit", "sha": ctx["sha"]}}]) as api:
            release.verify_remote_tag("repos/owner/repo", "v1.2.3", ctx)
            self.assertTrue(all(len(call.args) == 1 for call in api.call_args_list))
        for refs in ([], [dict(annotated, object=dict(type="commit", sha="c" * 40))]):
            with patch.object(release, "api", return_value=refs), self.assertRaises(ValueError):
                release.verify_remote_tag("repos/owner/repo", "v1.2.3", ctx)

    def test_pagination_includes_all_assets(self):
        with patch.object(release, "api", side_effect=[[{}] * 100, [{}]]) as api:
            self.assertEqual(len(release.api_pages("assets")), 101)
            self.assertIn("page=2", api.call_args.args[0])


if __name__ == "__main__":
    unittest.main()
