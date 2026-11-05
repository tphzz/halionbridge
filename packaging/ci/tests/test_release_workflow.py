"""Credential and entrypoint contracts, checked without executing a workflow."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


class ReleaseWorkflowTests(unittest.TestCase):
    def test_release_secret_is_scoped_to_the_single_release_step(self):
        text = (ROOT / ".github/workflows/native.yml").read_text(encoding="utf-8")
        steps = re.split(r"(?m)^      - ", text)
        release_steps = [step for step in steps if "packaging/ci/release.py draft " in step]
        self.assertEqual(len(release_steps), 1)
        expected = "GH_TOKEN: ${{ secrets.GH_TOKEN }}"
        self.assertIn(expected, release_steps[0])
        self.assertEqual(text.count("GH_TOKEN:"), 1)
        self.assertNotIn("GH_TOKEN:", steps[0])
        self.assertNotIn("contents: write", text)
        self.assertIn("actions: read", text)
        for workflow in (ROOT / ".github/workflows").glob("*.yml"):
            if workflow.name != "native.yml":
                other = workflow.read_text(encoding="utf-8")
                self.assertNotIn("GH_TOKEN:", other)
                self.assertNotIn("packaging/ci/release.py draft ", other)

    def test_full_build_keeps_scheduling_and_release_gate(self):
        text = (ROOT / ".github/workflows/native.yml").read_text(encoding="utf-8")
        jobs = dict(re.findall(r"(?ms)^  ([a-z0-9-]+):\n(.*?)(?=^  [a-z0-9-]+:|\Z)", text.split("jobs:\n", 1)[1]))
        self.assertIn("needs: checks", jobs["windows-x64"])
        self.assertIn("needs: checks", jobs["windows-arm64"])
        self.assertIn("needs: windows-arm64", jobs["macos"])
        self.assertIn("needs: [windows-x64, macos]", jobs["draft"])
        self.assertIn("if: ${{ !inputs.debug }}", jobs["draft"])
        self.assertIn("group: native-x64-host", jobs["draft"])
        self.assertIn("persist-credentials: false", jobs["draft"])
        self.assertIn("workflow_dispatch:", text)
        self.assertNotRegex(text, r"(?m)^  (push|pull_request|schedule):")
