"""Execute the Windows workflow snippets without Git Bash or VS initialization."""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
WORKFLOW = ROOT / ".github/workflows/native-platform.yml"


def windows_step(helper):
    workflow = WORKFLOW.read_text(encoding="utf-8")
    # Actionlint validates YAML separately. These tests execute the exact run
    # block rather than a second copy of the PowerShell invocation.
    for block in re.split(r"(?m)^      - ", workflow)[1:]:
        if helper not in block or "runner.os == 'macOS'" in block:
            continue
        if "shell: powershell" not in block:
            raise AssertionError("Windows steps must explicitly use Windows PowerShell")
        return textwrap.dedent(block.split("        run: |\n", 1)[1])
    raise AssertionError(f"Missing Windows workflow step for {helper}")


class CheckoutTests(unittest.TestCase):
    def test_long_paths_cover_checkout_and_subsequent_git_commands(self):
        job_environment = WORKFLOW.read_text(encoding="utf-8").split("    env:\n", 1)[1].split("    defaults:\n", 1)[0]
        for setting in ('GIT_CONFIG_COUNT: "1"', 'GIT_CONFIG_KEY_0: core.longpaths', 'GIT_CONFIG_VALUE_0: "true"'):
            self.assertIn(setting, job_environment)


@unittest.skipUnless(os.name == "nt", "Requires native Windows PowerShell")
class WindowsWorkflowTests(unittest.TestCase):
    def run_step(self, root, helper, **settings):
        script = root / "step.ps1"
        script.write_text(windows_step(helper), encoding="utf-8")
        system = Path(os.environ["SystemRoot"]) / "System32"
        env = {key: value for key, value in os.environ.items() if key.upper() != "PATH"}
        env.update(PATH=str(Path(sys.executable).parent),
                   PLATFORM="windows-x64", DEBUG_BUILD="false", NOTARIZE="false",
                   GITHUB_WORKSPACE=str(root), GITHUB_RUN_ID="123", GITHUB_RUN_ATTEMPT="1")
        env.update(settings)
        self.assertIsNone(shutil.which("bash", path=env["PATH"]))
        return subprocess.run([str(system / "WindowsPowerShell/v1.0/powershell.exe"),
                               "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                               "-File", str(script)], cwd=root, env=env, capture_output=True, text=True)

    def test_build_arguments_work_without_bash_and_preserve_failure(self):
        for platform, debug, notarize, exit_code in (
            ("windows-x64", "false", "false", 0),
            ("windows-arm64", "true", "false", 17),
            ("windows-x64", "true", "true", 2),
        ):
            with self.subTest(platform=platform), tempfile.TemporaryDirectory(prefix="ci windows ") as directory:
                root = Path(directory)
                helper = root / "packaging/ci/build.py"
                helper.parent.mkdir(parents=True)
                helper.write_text("import json, sys\nprint(json.dumps(sys.argv[1:]))\nsys.exit(" + str(exit_code) + ")\n")
                result = self.run_step(root, "packaging/ci/build.py", PLATFORM=platform,
                                       DEBUG_BUILD=debug, NOTARIZE=notarize)
                self.assertEqual(result.returncode, exit_code, result.stderr)
                expected = [platform] + (["--debug"] if debug == "true" else [])
                expected += ["--notarize"] if notarize == "true" else []
                self.assertEqual(json.loads(result.stdout), expected)

    def test_cleanup_works_after_setup_failure_and_preserves_sibling(self):
        with tempfile.TemporaryDirectory(prefix="ci windows ") as directory:
            root = Path(directory)
            checkout = root / ".ci/123-1-windows-x64"
            helper = checkout / "packaging/ci/cleanup.py"
            helper.parent.mkdir(parents=True)
            shutil.copyfile(ROOT / "packaging/ci/cleanup.py", helper)
            sibling = root / "other-job"
            sibling.write_text("preserve")
            result = self.run_step(root, "packaging/ci/cleanup.py")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse(checkout.exists())
            self.assertEqual(sibling.read_text(), "preserve")

    def test_cleanup_skips_missing_checkout(self):
        with tempfile.TemporaryDirectory(prefix="ci windows ") as directory:
            result = self.run_step(Path(directory), "packaging/ci/cleanup.py")
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_cleanup_failure_is_not_silenced(self):
        with tempfile.TemporaryDirectory(prefix="ci windows ") as directory:
            root = Path(directory)
            helper = root / ".ci/123-1-windows-x64/packaging/ci/cleanup.py"
            helper.parent.mkdir(parents=True)
            helper.write_text("import sys\nsys.exit(19)\n")
            result = self.run_step(root, "packaging/ci/cleanup.py")
            self.assertEqual(result.returncode, 19, result.stderr)
