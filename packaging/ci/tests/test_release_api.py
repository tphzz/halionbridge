"""Transport failures must fail closed without reading real credentials."""
import contextlib
import io
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import release
from release_backend import Backend
from test_release_packages import context, make_packages


class ReleaseApiTests(unittest.TestCase):
    def test_missing_token_never_uses_cached_cli_authentication(self):
        for token in ("", "  "):
            with self.subTest(token=token), patch.dict(os.environ, GH_TOKEN=token), \
                 patch.object(release.subprocess, "run") as command:
                with self.assertRaisesRegex(ValueError, "GH_TOKEN"):
                    release.api("repos/owner/repo/releases")
                command.assert_not_called()
                with patch.dict(os.environ, GITHUB_WORKFLOW="00 Full - All Platforms", GITHUB_EVENT_NAME="workflow_dispatch"), \
                     patch.object(release, "api") as api, self.assertRaises(ValueError):
                    release.draft(Path("unused"), context())
                api.assert_not_called()

    def test_http_failures_are_not_absence_and_do_not_echo_cli_diagnostics(self):
        for status in (401, 403, 404, 429, 500, 502):
            with self.subTest(status=status), patch.dict(os.environ, GH_TOKEN="offline-test-token"), \
                 patch.object(release.subprocess, "run", return_value=subprocess.CompletedProcess(
                     [], 1, stdout="", stderr=f"private-diagnostic (HTTP {status})")):
                with self.assertRaisesRegex(ValueError, f"HTTP {status}") as error:
                    release.api("repos/owner/repo/releases")
                self.assertNotIn("private-diagnostic", str(error.exception))

    def test_timeout_and_malformed_json_fail_closed(self):
        with patch.dict(os.environ, GH_TOKEN="offline-test-token"):
            with patch.object(release.subprocess, "run", side_effect=subprocess.TimeoutExpired("gh", 180)), \
                 self.assertRaisesRegex(ValueError, "uncertain"):
                release.api("repos/owner/repo/releases")
            with patch.object(release.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "not json", "")), \
                 self.assertRaises(ValueError):
                release.api("repos/owner/repo/releases")

    def test_upload_uses_binary_input_and_known_release_id(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "input.zip"
            path.write_bytes(b"bytes")
            endpoint = "https://uploads.github.com/repos/owner/repo/releases/123/assets?name=pending.zip"
            with patch.dict(os.environ, GH_TOKEN="offline-test-token"), \
                 patch.object(release.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, '{"id":1}', "")) as run:
                self.assertEqual(release.api(endpoint, "POST", binary=path), dict(id=1))
            args = run.call_args.args[0]
            self.assertEqual(args[:3], ["gh", "api", endpoint])
            self.assertIn("Content-Type: application/octet-stream", args)
            self.assertEqual(args[args.index("--input") + 1], str(path))
            self.assertNotIn("offline-test-token", args)

    def test_listing_errors_never_create_a_draft(self):
        for failure in (ValueError("HTTP 401"), OSError("network unavailable"), None, {}, [None]):
            with self.subTest(failure=type(failure).__name__), patch.dict(
                    os.environ, GH_TOKEN="offline-test-token", GITHUB_WORKFLOW="00 Full - All Platforms", GITHUB_EVENT_NAME="workflow_dispatch"):
                mock = patch.object(release, "api", side_effect=failure) if isinstance(failure, Exception) else patch.object(release, "api", return_value=failure)
                with mock as api, self.assertRaises((ValueError, OSError)):
                    release.draft(Path("unused"), context())
                self.assertEqual(len(api.call_args_list), 1)

    def test_reserved_snapshot_tag_namespace_fails_before_api_calls(self):
        for ref in ("refs/tags/snapshot", "refs/tags/snapshot/main"):
            with self.subTest(ref=ref), patch.dict(os.environ, GH_TOKEN="offline-test-token",
                    GITHUB_WORKFLOW="00 Full - All Platforms", GITHUB_EVENT_NAME="workflow_dispatch"), \
                 patch.object(release, "api") as api, self.assertRaisesRegex(ValueError, "reserved"):
                release.draft(Path("unused"), context(ref))
            api.assert_not_called()

    def test_report_uses_actual_url_in_stdout_and_step_summary(self):
        with tempfile.TemporaryDirectory() as directory:
            summary = Path(directory) / "summary.md"
            summary.write_text("existing\n", encoding="utf-8")
            result = dict(action="preserved", url="https://github.com/owner/repo/releases/tag/untagged-real")
            output = io.StringIO()
            with patch.dict(os.environ, GITHUB_STEP_SUMMARY=str(summary)), contextlib.redirect_stdout(output):
                release.report(result)
            self.assertEqual(output.getvalue(), "Release preserved: " + result["url"] + "\n")
            self.assertEqual(summary.read_text(), "existing\n" + output.getvalue())

    def test_invalid_packages_allow_no_release_writes(self):
        with tempfile.TemporaryDirectory() as directory:
            root, ctx = Path(directory), context()
            make_packages(root, ctx)
            (root / "unexpected.txt").write_text("bad input")
            backend = Backend(ctx, existing=False)
            with patch.dict(os.environ, GH_TOKEN="offline-test-token", GITHUB_WORKFLOW="00 Full - All Platforms",
                            GITHUB_EVENT_NAME="workflow_dispatch"), patch.object(release, "api", side_effect=backend.api), \
                 self.assertRaises(ValueError):
                release.draft(root, ctx)
            self.assertEqual(backend.mutations, [])

    def test_malformed_tag_objects_are_actionable_validation_failures(self):
        ctx = context("refs/tags/0.5.0")
        for obj in (None, {}, dict(sha=ctx["sha"]), dict(type="commit", sha=None), dict(type="tree", sha=ctx["sha"])):
            with self.subTest(obj=obj), patch.object(release, "api", return_value=[dict(ref=ctx["ref"], object=obj)]), \
                 self.assertRaises(ValueError):
                release.verify_remote_tag("repos/owner/repo", "0.5.0", ctx)

    def test_invalid_workflow_context_fails_before_accessing_files_or_network(self):
        env = dict(GITHUB_REF="refs/heads/main", GITHUB_SHA="a" * 40, GITHUB_REPOSITORY="owner/repo",
                   CONFIGURATION="Release", GITHUB_RUN_ID="1", GITHUB_RUN_NUMBER="1", GITHUB_RUN_ATTEMPT="1")
        cases = dict(GITHUB_REF="refs/heads/", GITHUB_SHA="invalid", GITHUB_REPOSITORY="owner/repo/other",
                     CONFIGURATION="Other", GITHUB_RUN_ID="0", GITHUB_RUN_NUMBER="-1", GITHUB_RUN_ATTEMPT="invalid")
        for name, value in cases.items():
            with self.subTest(name=name), patch.dict(os.environ, dict(env, **{name: value})), self.assertRaises(ValueError):
                release.context()
