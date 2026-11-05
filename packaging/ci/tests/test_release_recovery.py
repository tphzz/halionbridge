import copy
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import release
from release_backend import Backend
from test_release_packages import context, make_packages

ENV = dict(GITHUB_EVENT_NAME="workflow_dispatch", GITHUB_WORKFLOW="00 Full - All Platforms", GH_TOKEN="offline-test-token")


class RecoveryTests(unittest.TestCase):
    def invoke(self, root, ctx, backend):
        with patch.dict(os.environ, ENV), patch.object(release, "api", side_effect=backend.api), \
             patch.object(release.subprocess, "run", side_effect=AssertionError("Unexpected external command")):
            return release.draft(root, ctx)

    def assert_complete(self, backend, root, ctx):
        self.assertTrue(backend.current["draft"])
        self.assertEqual(backend.current["tag_name"], release.target(ctx))
        self.assertEqual(backend.current["target_commitish"], ctx["sha"])
        self.assertEqual(backend.current["body"], release.notes(ctx))
        names = {asset["name"] for asset in backend.assets}
        self.assertEqual(names, {path.name for path in root.iterdir() if path.suffix != ".sha256"})
        self.assertEqual(len(backend.assets), 7)
        for asset in backend.assets:
            self.assertEqual(asset["digest"], "sha256:" + release.digest(root / asset["name"]))
            self.assertEqual(asset["state"], "uploaded")

    def test_new_branch_and_same_commit_refresh_report_actual_url(self):
        with tempfile.TemporaryDirectory() as directory:
            root, ctx = Path(directory), context()
            make_packages(root, ctx)
            backend = Backend(ctx, existing=False)
            other = copy.deepcopy(backend.others)
            result = self.invoke(root, ctx, backend)
            self.assertEqual(result, dict(action="created", url=backend.current["html_url"]))
            self.assert_complete(backend, root, ctx)
            previous_names = {asset["name"] for asset in backend.assets}
            result = self.invoke(root, ctx, backend)
            self.assertEqual(result["action"], "refreshed")
            self.assert_complete(backend, root, ctx)
            self.assertEqual(other, backend.others)
            self.assertTrue(all(previous_names.issubset(set(names)) for names in backend.upload_checks[-7:]))
            checksums = (root / "SHA256SUMS.txt").read_text().splitlines()
            expected = sorted(path for path in root.iterdir() if path.suffix in (".json", ".zip"))
            self.assertEqual(checksums, [release.digest(path) + "  " + path.name for path in expected])

    def test_each_interrupted_branch_refresh_is_repairable(self):
        failures = [("upload", n) for n in range(1, 8)] + [("rename", n) for n in range(1, 8)]
        failures += [(stage, 1) for stage in ("metadata", "delete", "delete-response", "rename-response", "complete", "complete-response", "upload-response")]
        for stage, occurrence in failures:
            with self.subTest(stage=stage, occurrence=occurrence), tempfile.TemporaryDirectory() as directory:
                root, ctx = Path(directory), context()
                make_packages(root, ctx)
                backend = Backend(ctx)
                backend.fail(stage, occurrence)
                with self.assertRaises(ValueError):
                    self.invoke(root, ctx, backend)
                self.assertTrue(backend.current["draft"])
                if stage.startswith("upload"):
                    self.assertIn("old.zip", [asset["name"] for asset in backend.assets])
                self.invoke(root, ctx, backend)
                self.assert_complete(backend, root, ctx)

    def test_existing_tag_releases_are_preserved_regardless_of_ownership_or_state(self):
        for published in (False, True):
            for body in ("manual notes", "incomplete", None):
                with self.subTest(published=published, body=body), tempfile.TemporaryDirectory() as directory:
                    root, ctx = Path(directory), context("refs/tags/0.3.1")
                    make_packages(root, ctx)
                    backend = Backend(ctx)
                    backend.current.update(draft=not published, body=body)
                    before = copy.deepcopy((backend.current, backend.assets, backend.refs))
                    self.assertEqual(self.invoke(root, ctx, backend)["action"], "preserved")
                    self.assertEqual(before, (backend.current, backend.assets, backend.refs))
                    self.assertEqual(backend.mutations, [])

    def test_failed_initial_tag_draft_is_deleted_but_tag_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            root, ctx = Path(directory), context("refs/tags/v0.5.0")
            make_packages(root, ctx)
            backend = Backend(ctx, existing=False)
            refs = copy.deepcopy(backend.refs)
            backend.fail("upload", 2)
            with self.assertRaisesRegex(ValueError, "newly created draft"):
                self.invoke(root, ctx, backend)
            self.assertIsNone(backend.current)
            self.assertEqual(backend.refs, refs)
            self.assertEqual(self.invoke(root, ctx, backend)["action"], "created")
            self.assert_complete(backend, root, ctx)

    def test_cleanup_failure_preserves_incomplete_tag_and_reports_manual_recovery(self):
        with tempfile.TemporaryDirectory() as directory:
            root, ctx = Path(directory), context("refs/tags/0.5.0")
            make_packages(root, ctx)
            backend = Backend(ctx, existing=False)
            backend.fail("upload")
            backend.fail("cleanup")
            with self.assertRaisesRegex(ValueError, "Manual recovery"):
                self.invoke(root, ctx, backend)
            before = copy.deepcopy((backend.current, backend.assets))
            self.assertEqual(self.invoke(root, ctx, backend)["action"], "preserved")
            self.assertEqual(before, (backend.current, backend.assets))

    def test_uncertain_creation_is_not_blindly_retried_or_deleted(self):
        with tempfile.TemporaryDirectory() as directory:
            root, ctx = Path(directory), context("refs/tags/0.5.0")
            make_packages(root, ctx)
            backend = Backend(ctx, existing=False)
            backend.fail("create-response")
            with self.assertRaisesRegex(ValueError, "uncertain"):
                self.invoke(root, ctx, backend)
            self.assertIsNotNone(backend.current)
            self.assertEqual(len([call for call in backend.mutations if call[1] == "POST"]), 1)
            self.assertFalse(any(call[1] == "DELETE" for call in backend.mutations))

    def test_legacy_placeholder_draft_requires_reconciliation(self):
        with tempfile.TemporaryDirectory() as directory:
            root, ctx = Path(directory), context()
            make_packages(root, ctx)
            backend = Backend(ctx)
            backend.current.update(tag_name="untagged-example", body="<!-- halionbridge-ci-v1 " + json.dumps(ctx) + " -->")
            with self.assertRaisesRegex(ValueError, "reconcil"):
                self.invoke(root, ctx, backend)
            self.assertEqual(backend.mutations, [])

    def test_duplicate_release_on_later_page_fails_without_writes(self):
        with tempfile.TemporaryDirectory() as directory:
            root, ctx = Path(directory), context()
            make_packages(root, ctx)
            backend = Backend(ctx)
            backend.others = [dict(id=i, tag_name="other-" + str(i), body="", draft=False) for i in range(2, 102)]
            backend.others.append(dict(backend.current, id=102))
            with self.assertRaisesRegex(ValueError, "Multiple"):
                self.invoke(root, ctx, backend)
            self.assertEqual(backend.mutations, [])

    def test_published_snapshot_and_snapshot_refs_fail_without_writes(self):
        for conflict in ("published", "same-commit-tag", "parent-tag", "unowned", "stale"):
            with self.subTest(conflict=conflict), tempfile.TemporaryDirectory() as directory:
                root, ctx = Path(directory), context()
                make_packages(root, ctx)
                backend = Backend(ctx)
                if conflict == "published":
                    backend.current["draft"] = False
                elif conflict in ("same-commit-tag", "parent-tag"):
                    name = release.target(ctx) if conflict == "same-commit-tag" else "snapshot/feature"
                    backend.refs = [dict(ref="refs/tags/" + name, object=dict(type="commit", sha=ctx["sha"]))]
                elif conflict == "stale":
                    backend.current["body"] = release.notes(dict(ctx, run_number=99))
                else:
                    backend.current["body"] = "manual"
                with self.assertRaises(ValueError):
                    self.invoke(root, ctx, backend)
                self.assertEqual(backend.mutations, [])

    def test_final_inventory_failure_is_incomplete_until_successful_retry(self):
        with tempfile.TemporaryDirectory() as directory:
            root, ctx = Path(directory), context()
            make_packages(root, ctx)
            backend = Backend(ctx)
            inject = [True]

            def api(endpoint, method="GET", payload=None, **kwargs):
                result = backend.api(endpoint, method, payload, **kwargs)
                if inject[0] and method == "GET" and "/assets?" in endpoint and backend.counts.get("rename", 0) == 7:
                    inject[0] = False
                    result.append(dict(id=999, name="extra.zip", state="uploaded", size=1, digest="bad"))
                return result

            with patch.dict(os.environ, ENV), patch.object(release, "api", side_effect=api), self.assertRaisesRegex(ValueError, "inventory"):
                release.draft(root, ctx)
            self.assertEqual(backend.current["body"], release.notes(ctx, complete=False))
            self.assertNotIn("complete", backend.counts)
            self.invoke(root, ctx, backend)
            self.assert_complete(backend, root, ctx)

    def test_new_tag_draft_supports_annotated_tags_and_preserves_refs(self):
        with tempfile.TemporaryDirectory() as directory:
            root, ctx = Path(directory), context("refs/tags/v0.5.0-rc.1")
            make_packages(root, ctx)
            backend = Backend(ctx, existing=False)
            backend.refs[0]["object"] = dict(type="tag", sha="b" * 40)
            backend.tag_objects["b" * 40] = dict(object=dict(type="commit", sha=ctx["sha"]))
            refs = copy.deepcopy(backend.refs)
            self.invoke(root, ctx, backend)
            self.assert_complete(backend, root, ctx)
            self.assertTrue(backend.current["prerelease"])
            self.assertEqual(backend.refs, refs)

    def test_later_asset_pages_are_removed_only_after_complete_upload(self):
        with tempfile.TemporaryDirectory() as directory:
            root, ctx = Path(directory), context()
            make_packages(root, ctx)
            backend = Backend(ctx)
            backend.assets += [dict(backend.assets[0], id=i, name=f"old-{i}.zip") for i in range(100, 201)]
            old = {asset["name"] for asset in backend.assets}
            self.invoke(root, ctx, backend)
            self.assertTrue(all(old.issubset(set(names)) for names in backend.upload_checks))
            self.assert_complete(backend, root, ctx)

    def test_cleanup_refuses_a_tag_draft_published_during_upload(self):
        with tempfile.TemporaryDirectory() as directory:
            root, ctx = Path(directory), context("refs/tags/0.5.0")
            make_packages(root, ctx)
            backend = Backend(ctx, existing=False)

            def api(endpoint, method="GET", payload=None, **kwargs):
                if kwargs.get("binary") is not None:
                    backend.current["draft"] = False
                    raise ValueError("upload interrupted by manual publication")
                return backend.api(endpoint, method, payload, **kwargs)

            with patch.dict(os.environ, ENV), patch.object(release, "api", side_effect=api), self.assertRaisesRegex(ValueError, "Manual recovery"):
                release.draft(root, ctx)
            self.assertFalse(backend.current["draft"])
            self.assertFalse(any(method == "DELETE" for _, method, _ in backend.mutations))

    def test_owned_placeholder_is_repaired_by_id_without_another_release(self):
        with tempfile.TemporaryDirectory() as directory:
            root, ctx = Path(directory), context()
            make_packages(root, ctx)
            backend = Backend(ctx)
            backend.current["tag_name"] = "untagged-recoverable"
            self.assertEqual(self.invoke(root, ctx, backend)["action"], "refreshed")
            self.assertFalse(any(endpoint.endswith("/releases") and method == "POST" for endpoint, method, _ in backend.mutations))
            self.assert_complete(backend, root, ctx)

    def test_manually_reconciled_legacy_marker_can_refresh(self):
        with tempfile.TemporaryDirectory() as directory:
            root, ctx = Path(directory), context()
            make_packages(root, ctx)
            backend = Backend(ctx)
            backend.current["body"] = "<!-- halionbridge-ci-v1 " + json.dumps(dict(ctx, run_number=1)) + " -->"
            self.invoke(root, ctx, backend)
            self.assert_complete(backend, root, ctx)

    def test_other_branch_and_tag_releases_survive_new_commit_refresh(self):
        with tempfile.TemporaryDirectory() as directory:
            root, ctx = Path(directory), context()
            make_packages(root, ctx)
            backend = Backend(ctx)
            backend.others += [dict(id=3, tag_name="snapshot/feature-foo", body=release.notes(context("refs/heads/feature-foo")), draft=True),
                               dict(id=4, tag_name="0.5.0", body="version release", draft=False)]
            others = copy.deepcopy(backend.others)
            self.invoke(root, ctx, backend)
            new_ctx = dict(ctx, sha="c" * 40, run_number=3, run_id=124)
            for path in root.iterdir():
                path.unlink()
            make_packages(root, new_ctx)
            self.invoke(root, new_ctx, backend)
            self.assertEqual(backend.others, others)
            self.assert_complete(backend, root, new_ctx)

    def test_invalid_upload_url_fails_as_an_incomplete_draft(self):
        for url in (None, "https://example.invalid/assets", "https://uploads.github.com/repos/other/repo/releases/1/assets"):
            with self.subTest(url=url), tempfile.TemporaryDirectory() as directory:
                root, ctx = Path(directory), context()
                make_packages(root, ctx)
                backend = Backend(ctx)
                backend.current["upload_url"] = url
                with self.assertRaisesRegex(ValueError, "upload URL"):
                    self.invoke(root, ctx, backend)
                self.assertEqual(backend.current["body"], release.notes(ctx, complete=False))
                self.assertIn("old.zip", [asset["name"] for asset in backend.assets])
                self.assertEqual(backend.upload_checks, [])

    def test_upload_size_state_hash_and_identity_must_verify_before_deletion(self):
        for field, value in (("size", 0), ("state", "starter"), ("digest", "sha256:" + "0" * 64), ("id", True)):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as directory:
                root, ctx = Path(directory), context()
                make_packages(root, ctx)
                backend = Backend(ctx)

                def api(endpoint, method="GET", payload=None, **kwargs):
                    result = backend.api(endpoint, method, payload, **kwargs)
                    if method == "GET" and "/assets?" in endpoint:
                        result[-1][field] = value
                    return result

                with patch.dict(os.environ, ENV), patch.object(release, "api", side_effect=api), self.assertRaises(ValueError):
                    release.draft(root, ctx)
                self.assertIn("old.zip", [asset["name"] for asset in backend.assets])
                self.assertFalse(any(method == "DELETE" for _, method, _ in backend.mutations))
                self.invoke(root, ctx, backend)
                self.assert_complete(backend, root, ctx)
