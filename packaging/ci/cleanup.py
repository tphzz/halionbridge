"""Remove only this run's isolated checkout, never the runner workspace itself."""
import os
import re
import shutil
import stat
import sys
from pathlib import Path

def owned_path(workspace, run_id, attempt, job):
    if not re.fullmatch(r"[0-9]+", run_id) or not re.fullmatch(r"[0-9]+", attempt):
        raise ValueError("Invalid workflow run identity")
    if job not in ("checks", "draft", "windows-x64", "windows-arm64", "macos-universal"):
        raise ValueError("Invalid checkout identity")
    root = Path(workspace).resolve()
    path = root / ".ci" / f"{run_id}-{attempt}-{job}"
    if (root / ".ci").is_symlink() or path.is_symlink() or path.resolve().parent != root / ".ci":
        raise ValueError("Checkout resolves outside its owned location")
    return path


def remove_checkout(path):
    # Git's core.longpaths only affects Git. Use extended-length paths for all
    # Python filesystem operations too, independently of the host registry policy.
    # owned_path has already validated the resolved checkout boundary.
    if os.name == "nt":
        name = os.path.abspath(path)
        if not name.startswith("\\\\?\\"):
            name = "\\\\?\\UNC\\" + name[2:] if name.startswith("\\\\") else "\\\\?\\" + name
        path = Path(name)

    def retry_readonly(function, filename, error):
        target = Path(filename)
        if (os.name != "nt" or not isinstance(error[1], PermissionError)
                or not target.resolve().is_relative_to(path.resolve())
                or target.is_symlink()):
            raise error[1]
        # Git object files are read-only on Windows. Only the already validated
        # checkout may have that attribute cleared; never retry a workspace path.
        os.chmod(target, stat.S_IREAD | stat.S_IWRITE)
        function(filename)

    if path.exists():
        shutil.rmtree(path, onerror=retry_readonly)


if __name__ == "__main__":
    path = owned_path(os.environ["GITHUB_WORKSPACE"], os.environ["GITHUB_RUN_ID"],
                      os.environ["GITHUB_RUN_ATTEMPT"], sys.argv[1])
    remove_checkout(path)
