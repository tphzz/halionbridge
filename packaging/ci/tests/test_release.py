import json
import os
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import release


class ReleaseTests(unittest.TestCase):
    def context(self, ref="refs/heads/main", run=2):
        return dict(repository="owner/repo", ref=ref, sha="a" * 40,
                    run_id=123, run_number=run, run_attempt=1, configuration="Release")

    def test_branch_targets_are_stable_and_collision_resistant(self):
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

    def test_verifies_complete_set_and_rejects_tampering(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for platform in release.PLATFORMS:
                package = root / (platform + ".zip")
                with zipfile.ZipFile(package, "w") as archive:
                    archive.writestr("halionbridge.exe" if platform.startswith("windows") else "halionbridge", "binary")
                    archive.writestr("examples/test/halionbridge_build.lua", "return {}")
                    for name in release.REQUIRED_NOTICES:
                        archive.writestr(name, "notice")
                manifest = dict(self.context(), platform=platform, archive=package.name,
                                sha256=release.digest(package))
                (root / (platform + ".json")).write_text(json.dumps(manifest))
            self.assertEqual(len(release.verify_assets(root, self.context())), 3)
            for key, value in (("sha", "b" * 40), ("run_attempt", 2), ("configuration", "Debug")):
                with self.subTest(key=key), self.assertRaises(ValueError):
                    release.verify_assets(root, dict(self.context(), **{key: value}))
            (root / "windows-x64.zip").write_bytes(b"tampered")
            with self.assertRaises(ValueError):
                release.verify_assets(root, self.context())

    def test_partial_set_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError):
                release.verify_assets(Path(directory), self.context())

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

    def test_new_branch_creates_draft_without_publishing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            ctx = self.context()
            for platform in release.PLATFORMS:
                (root / (platform + ".json")).write_text("{}")
            backend = FakeGitHub(ctx, self.context(run=1))
            backend.current = None
            with patch.dict(os.environ, GITHUB_EVENT_NAME="workflow_dispatch", GITHUB_WORKFLOW="00 Full - All Platforms"), \
                 patch.object(release, "verify_assets", return_value=[]), \
                 patch.object(release, "api", side_effect=backend.api), \
                 patch.object(release.subprocess, "run", side_effect=backend.upload):
                release.draft(root, ctx)
            self.assertTrue(backend.current["draft"])
            self.assertTrue(backend.current["prerelease"])
            self.assertEqual(backend.current["target_commitish"], ctx["sha"])
            self.assertEqual(backend.current["tag_name"], release.target(ctx))
            self.assertTrue(all(payload.get("draft", True) for _, _, payload in backend.mutations))

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

    def test_failed_upload_retains_previous_assets_and_retry_repairs_draft(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archives = []
            for platform in release.PLATFORMS:
                archive = root / (platform + ".zip")
                archive.write_bytes(b"package")
                (root / (platform + ".json")).write_text("{}")
                archives.append(archive)
            ctx = self.context()
            backend = FakeGitHub(ctx, self.context(run=1))
            backend.fail_upload = 2
            env = dict(GITHUB_EVENT_NAME="workflow_dispatch", GITHUB_WORKFLOW="00 Full - All Platforms")
            with patch.dict(os.environ, env), patch.object(release, "verify_assets", return_value=archives), \
                 patch.object(release, "api", side_effect=backend.api), \
                 patch.object(release.subprocess, "run", side_effect=backend.upload):
                with self.assertRaises(subprocess.CalledProcessError):
                    release.draft(root, ctx)
                self.assertIn("old.zip", [a["name"] for a in backend.assets])
                self.assertTrue(backend.current["draft"])
                backend.fail_upload = 0
                release.draft(root, ctx)
            self.assertEqual(len(backend.assets), 7)
            self.assertNotIn("old.zip", [a["name"] for a in backend.assets])
            self.assertFalse(any(a["name"].startswith("pending-") for a in backend.assets))
            self.assertEqual(backend.current["body"], release.notes(ctx))
            self.assertEqual(backend.other["body"], "unrelated")
            self.assertTrue(all(payload.get("draft", True) for _, _, payload in backend.mutations))

    def test_published_unowned_and_moved_tag_refuse_before_any_mutation(self):
        for scenario in ("published", "unowned", "moved"):
            with self.subTest(scenario=scenario), tempfile.TemporaryDirectory() as directory:
                ctx = self.context()
                backend = FakeGitHub(ctx, self.context(run=1))
                if scenario == "published":
                    backend.current["draft"] = False
                elif scenario == "unowned":
                    backend.current["body"] = "manual draft"
                else:
                    backend.refs = [dict(ref="refs/tags/" + release.target(ctx), object=dict(type="commit", sha="c" * 40))]
                with patch.dict(os.environ, GITHUB_EVENT_NAME="workflow_dispatch", GITHUB_WORKFLOW="00 Full - All Platforms"), \
                     patch.object(release, "verify_assets", return_value=[]), patch.object(release, "api", side_effect=backend.api), \
                     self.assertRaises(ValueError):
                    release.draft(Path(directory), ctx)
                self.assertEqual(backend.mutations, [])


class FakeGitHub:
    """Offline API model: no test reads credentials or contacts GitHub."""
    def __init__(self, ctx, previous):
        self.current = dict(id=1, tag_name=release.target(ctx), draft=True, body=release.notes(previous))
        self.other = dict(id=2, tag_name="unrelated", draft=False, body="unrelated")
        self.assets = [dict(id=10, name="old.zip")]
        self.refs = []
        self.mutations = []
        self.uploads = 0
        self.fail_upload = 0
        self.next_id = 20

    def api(self, endpoint, method="GET", payload=None):
        if method != "GET":
            self.mutations.append((endpoint, method, payload or {}))
        if "/git/matching-refs/" in endpoint:
            return self.refs
        if endpoint.endswith("releases?per_page=100&page=1"):
            return ([dict(self.current)] if self.current else []) + [dict(self.other)]
        if endpoint.endswith("/releases") and method == "POST":
            self.current = dict(id=1, **payload)
            self.assets = []
            return dict(self.current)
        if endpoint.endswith("releases/1"):
            if method == "PATCH":
                self.current.update(payload)
            return dict(self.current)
        if endpoint.endswith("releases/1/assets?per_page=100&page=1"):
            return [dict(asset) for asset in self.assets]
        if "/releases/assets/" in endpoint:
            asset_id = int(endpoint.rsplit("/", 1)[1])
            asset = next(a for a in self.assets if a["id"] == asset_id)
            if method == "DELETE":
                self.assets.remove(asset)
                return None
            if method == "PATCH":
                asset.update(payload)
                return dict(asset)
        raise AssertionError(f"Unexpected API call {method} {endpoint}")

    def upload(self, args, **kwargs):
        self.uploads += 1
        if self.uploads == self.fail_upload:
            raise subprocess.CalledProcessError(1, "simulated failed upload")
        path = Path(args[4])
        self.assets.append(dict(id=self.next_id, name=path.name, digest="sha256:" + release.digest(path)))
        self.next_id += 1


if __name__ == "__main__":
    unittest.main()
