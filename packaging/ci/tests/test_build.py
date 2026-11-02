import os
import subprocess
import stat
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build
import cleanup
import macos
import lint

ROOT = Path(__file__).resolve().parents[3]


class BuildTests(unittest.TestCase):
    def test_dependency_validation_rejects_non_system_libraries(self):
        build.validate_macos_dependencies("binary:\n\t/usr/lib/libSystem.B.dylib (compatibility version 1.0)\n")
        for text in ("binary:\n", "binary:\n\t@rpath/libjuce.dylib (compatibility version 1.0)\n"):
            with self.assertRaises(ValueError):
                build.validate_macos_dependencies(text)

    def test_cleanup_removes_readonly_git_objects_without_touching_sibling(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            owned = cleanup.owned_path(root, "1", "1", "checks")
            objects = owned / ".git/objects/aa"
            objects.mkdir(parents=True)
            readonly = objects / "fixture"
            readonly.write_text("object")
            readonly.chmod(stat.S_IREAD)
            sibling = root / "other-work"
            sibling.write_text("preserve")
            cleanup.remove_checkout(owned)
            self.assertFalse(owned.exists())
            self.assertEqual(sibling.read_text(), "preserve")

    def test_lint_queue_exception_does_not_allow_invalid_values(self):
        lint.check_queue_values("  queue: max\n  cancel-in-progress: false")
        for text in ("queue: typo", "cancel-in-progress: true"):
            with self.assertRaises(ValueError):
                lint.check_queue_values(text)

    def test_notarization_requires_accepted_status(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for status, code in (("Accepted", 0), ("Invalid", 0), ("Accepted", 1)):
                with self.subTest(status=status, code=code), \
                     patch.object(macos.subprocess, "run", return_value=subprocess.CompletedProcess([], code, '{"status":"' + status + '"}')) as command:
                    if status == "Accepted" and code == 0:
                        macos.notarize(root / "package.zip", root / "keychain", root)
                    else:
                        with self.assertRaises(ValueError):
                            macos.notarize(root / "package.zip", root / "keychain", root)
                    self.assertIn("--wait", command.call_args.args[0])
                    self.assertNotIn("stapler", command.call_args.args[0])

    def test_parallelism_is_bounded_to_positive_integer_input(self):
        self.assertEqual(build.parallelism("2"), "2")
        for value in ("", "0", "-1", "auto", "2; echo bad"):
            with self.assertRaises(ValueError):
                build.parallelism(value)

    def test_signing_identity_must_be_unique(self):
        entry = '1) ' + 'A' * 40 + ' "Developer ID Application: Example (ABCDEFGHIJ)"'
        self.assertEqual(macos.identity(entry + "\n" + entry), "A" * 40)
        for text in ("", entry + "\n" + entry.replace("A" * 40, "B" * 40)):
            with self.assertRaises(ValueError):
                macos.identity(text)

    def test_cleanup_cannot_escape_owned_checkout(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(cleanup.owned_path(directory, "1", "2", "checks").name, "1-2-checks")
            for run, attempt, job in [("../", "2", "checks"), ("1", "", "checks"), ("1", "2", "../")]:
                with self.assertRaises(ValueError):
                    cleanup.owned_path(directory, run, attempt, job)

    def test_detached_ci_branch_wins_over_tag(self):
        # All Git mutations here are confined to a disposable test fixture.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            def git(*args):
                return subprocess.check_output(["git", "-C", directory, *args], stderr=subprocess.STDOUT, text=True).strip()
            git("init", "-b", "main")
            git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "commit", "--allow-empty", "-m", "fixture")
            git("tag", "v1.2.3")
            sha = git("rev-parse", "HEAD")
            git("switch", "--detach", "HEAD")
            script = root / "probe.cmake"
            script.write_text(f'include("{(ROOT / "cmake/halionbridge_version.cmake").as_posix()}")\n'
                              f'halionbridge_configure_git_version("{root.as_posix()}")\n'
                              'message("IDENTITY=${HALIONBRIDGE_GIT_BRANCH}|${HALIONBRIDGE_GIT_TAG}")\n')
            env = dict(os.environ, GITHUB_ACTIONS="true", GITHUB_REF_TYPE="branch", GITHUB_REF_NAME="feature/ci", GITHUB_SHA=sha)
            result = subprocess.run(["cmake", "-P", str(script)], env=env, capture_output=True, text=True, check=True)
            self.assertIn("IDENTITY=feature-ci|", result.stderr)
            env.update(GITHUB_REF_TYPE="tag", GITHUB_REF_NAME="v1.2.3")
            result = subprocess.run(["cmake", "-P", str(script)], env=env, capture_output=True, text=True, check=True)
            self.assertIn("IDENTITY=v1.2.3|v1.2.3", result.stderr)
            env.update(GITHUB_REF_TYPE="branch", GITHUB_REF_NAME="wrong", GITHUB_SHA="0" * 40)
            result = subprocess.run(["cmake", "-P", str(script)], env=env, capture_output=True, text=True, check=True)
            self.assertIn("IDENTITY=v1.2.3|v1.2.3", result.stderr)


if __name__ == "__main__":
    unittest.main()
