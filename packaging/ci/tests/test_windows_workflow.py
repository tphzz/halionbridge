"""Execute the Windows workflow snippets without Git Bash or VS initialization."""
import json
import os
import re
import shutil
import subprocess
import stat
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import cleanup

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
    def run_script(self, root, source, **settings):
        script = root / "step.ps1"
        script.write_text(source, encoding="utf-8")
        system = Path(os.environ["SystemRoot"]) / "System32"
        # Subprocess fixtures must never inherit credentials from the test host.
        runtime_names = {"SYSTEMROOT", "WINDIR", "TEMP", "TMP", "USERPROFILE",
                         "PATHEXT", "COMSPEC", "LOCALAPPDATA", "APPDATA"}
        env = {key: value for key, value in os.environ.items() if key.upper() in runtime_names}
        env.update(PATH=str(Path(sys.executable).parent),
                   PLATFORM="windows-x64", DEBUG_BUILD="false", NOTARIZE="false",
                   GITHUB_WORKSPACE=str(root), GITHUB_RUN_ID="123", GITHUB_RUN_ATTEMPT="1")
        env.update(settings)
        self.assertIsNone(shutil.which("bash", path=env["PATH"]))
        return subprocess.run([str(system / "WindowsPowerShell/v1.0/powershell.exe"),
                               "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                               "-File", str(script)], cwd=root, env=env, capture_output=True, text=True)

    def run_step(self, root, helper, **settings):
        return self.run_script(root, windows_step(helper), **settings)

    def test_toolchain_export_excludes_unrelated_environment(self):
        source = (ROOT / "packaging/ci/windows.ps1").read_text(encoding="utf-8")
        # Execute just the environment export, without loading VS on the test host.
        export = source.split("if ($env:VisualStudioVersion -notlike '18.*')", 1)[1].split("\n", 1)[1]
        with tempfile.TemporaryDirectory(prefix="ci environment ") as directory:
            root = Path(directory)
            output = root / "github-env"
            result = self.run_script(root, export, GITHUB_ENV=str(output),
                                     CI_TEST_REGISTRATION_TOKEN="fixture-do-not-export",
                                     INCLUDE="fixture-include", LIB="fixture-lib",
                                     VSCMD_ARG_TGT_ARCH="arm64", VisualStudioVersion="18.0",
                                     VSCMD_ARG_HOST_ARCH="arm64", WindowsSdkDir="fixture-sdk",
                                     LIBPATH="invalid\nINJECTED=bad")
            self.assertEqual(result.returncode, 0, result.stderr)
            values = dict(line.split("=", 1) for line in output.read_text(encoding="utf-8-sig").splitlines())
            self.assertTrue("CI_TEST_REGISTRATION_TOKEN" not in values,
                            "Unrelated environment variable was exported")
            self.assertNotIn("GITHUB_ENV", values)
            self.assertNotIn("PLATFORM", values)
            self.assertNotIn("LIBPATH", values)
            self.assertNotIn("INJECTED", values)
            self.assertEqual(values["INCLUDE"], "fixture-include")
            self.assertEqual(values["LIB"], "fixture-lib")
            self.assertEqual(values["VSCMD_ARG_TGT_ARCH"], "arm64")
            self.assertEqual(values["VSCMD_ARG_HOST_ARCH"], "arm64")
            self.assertEqual(values["VisualStudioVersion"], "18.0")
            self.assertEqual(values["WindowsSdkDir"], "fixture-sdk")
            self.assertTrue(any(name.lower() == "path" for name in values))

    def test_cleanup_removes_long_readonly_pack_paths_without_host_long_path_policy(self):
        with tempfile.TemporaryDirectory(prefix="ci cleanup ") as directory:
            root = Path(directory)
            owned = cleanup.owned_path(root, "41522722411", "1", "windows-arm64")
            # The directory itself fits MAX_PATH; the pack filename does not.
            pack_dir = owned / ("nested-" + "x" * (224 - len(str(owned)))) / "objects/pack"
            extended = Path("\\\\?\\" + str(pack_dir))
            extended.mkdir(parents=True)
            for suffix in (".pack", ".idx", ".rev"):
                obj = extended / ("pack-" + "a" * 40 + suffix)
                self.assertLess(len(str(pack_dir)), 260)
                self.assertGreaterEqual(len(str(obj)) - 4, 260)
                obj.write_bytes(b"git object fixture")
                obj.chmod(stat.S_IREAD)
            sibling = root / "other-job"
            sibling.write_text("preserve")
            unlink = os.unlink

            def legacy_unlink(filename, *args, **kwargs):
                text = os.fspath(filename)
                # Emulate the Win32 path-policy failure even on hosts that have
                # long paths enabled. Python 3.13 rmtree skips this error, then
                # fails to remove the still-nonempty pack directory (WinError 145).
                if not text.startswith("\\\\?\\") and len(text) >= 260:
                    raise FileNotFoundError(2, "Legacy path limit", text, 3)
                return unlink(filename, *args, **kwargs)

            with patch.object(cleanup.os, "unlink", side_effect=legacy_unlink):
                cleanup.remove_checkout(owned)
            self.assertFalse(owned.exists())
            self.assertEqual(sibling.read_text(), "preserve")

    def test_cleanup_handles_unc_and_already_extended_paths(self):
        for source, expected in (
            (r"C:\workspace\.ci\1-1-checks", r"\\?\C:\workspace\.ci\1-1-checks"),
            (r"\\server\share\.ci\1-1-checks", r"\\?\UNC\server\share\.ci\1-1-checks"),
            (r"\\?\C:\workspace\.ci\1-1-checks", r"\\?\C:\workspace\.ci\1-1-checks"),
            (r"\\?\UNC\server\share\.ci\1-1-checks", r"\\?\UNC\server\share\.ci\1-1-checks"),
        ):
            # No access to a real share: check the rmtree boundary representation.
            with self.subTest(source=source), patch.object(Path, "exists", return_value=True), \
                 patch.object(cleanup.shutil, "rmtree") as remove:
                cleanup.remove_checkout(Path(source))
                self.assertEqual(str(remove.call_args.args[0]), expected)

    def test_cleanup_still_fails_when_a_file_cannot_be_removed(self):
        with tempfile.TemporaryDirectory(prefix="ci cleanup ") as directory:
            owned = cleanup.owned_path(directory, "1", "1", "checks")
            owned.mkdir(parents=True)
            retained = owned / "retained"
            retained.write_text("preserve on failure")
            # A persistent nonempty directory must fail; never hide error 145.
            with patch.object(cleanup.os, "unlink", return_value=None), \
                 self.assertRaises(OSError) as failure:
                cleanup.remove_checkout(owned)
            self.assertEqual(failure.exception.winerror, 145)
            self.assertTrue(retained.exists())

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
