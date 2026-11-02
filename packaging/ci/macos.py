"""Use an existing signing keychain without changing its persistent configuration."""
import json
import os
import re
import stat
import subprocess
from pathlib import Path


def run(*args):
    return subprocess.check_output(args, text=True).strip()


def identity(text):
    identities = set(re.findall(r'([A-Fa-f0-9]{40})\s+"Developer ID Application: [^\n]+ \([A-Z0-9]{10}\)"', text))
    if len(identities) != 1:
        raise ValueError(f"Expected exactly one Developer ID Application identity; found {len(identities)}")
    return identities.pop()


def sign(binary: Path, entitlements: Path, scratch: Path):
    keychain = Path.home() / "Library/Keychains/login.keychain-db"
    password_file = Path.home() / ".config/developer-id-signing/login-keychain-password"
    if not keychain.is_file() or not password_file.is_file() or password_file.is_symlink():
        raise ValueError("Persistent signing keychain or protected password file is missing")
    info = password_file.stat()
    if info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) != 0o600:
        raise ValueError("Signing password file must be owned by the runner user with mode 600")
    password = password_file.read_text().rstrip("\n")
    if not password or "\n" in password or "\r" in password:
        raise ValueError("Signing password file must contain one nonempty line")
    print("::add-mask::" + password.replace("%", "%25"), flush=True)
    # Do not include the credential-bearing command in an exception or log.
    unlocked = subprocess.run(["security", "unlock-keychain", "-p", password, str(keychain)],
                              capture_output=True)
    del password
    if unlocked.returncode:
        raise ValueError("Could not unlock the existing signing keychain")
    fingerprint = identity(run("security", "find-identity", "-v", "-p", "codesigning", str(keychain)))
    scratch.mkdir(parents=True, exist_ok=True)
    source = scratch / "signing-check.c"
    source.write_text("int main(void) { return 0; }\n")
    probe = scratch / "signing-check"
    run("xcrun", "clang", "-mmacosx-version-min=13.3", str(source), "-o", str(probe))
    for executable in (probe, binary):
        run("codesign", "--force", "--timestamp", "--options", "runtime", "--keychain", str(keychain),
            "--sign", fingerprint, "--entitlements", str(entitlements), str(executable))
        run("codesign", "--verify", "--strict", "--verbose=2", str(executable))
    run(str(probe))
    return keychain


def notarize(archive: Path, keychain: Path, scratch: Path):
    command = ["xcrun", "notarytool", "submit", str(archive), "--keychain-profile", "developer-id-notary",
               "--keychain", str(keychain), "--wait", "--output-format", "json"]
    completed = subprocess.run(command, capture_output=True, text=True)
    (scratch / "notary-result.json").write_text(completed.stdout or "", encoding="utf-8")
    if completed.stderr:
        (scratch / "notary-stderr.txt").write_text(completed.stderr, encoding="utf-8")
    result = json.loads(completed.stdout) if completed.stdout.strip() else {}
    if completed.returncode or result.get("status") != "Accepted":
        if result.get("id"):
            log = run("xcrun", "notarytool", "log", result["id"], "--keychain-profile", "developer-id-notary",
                      "--keychain", str(keychain))
            (scratch / "notary-log.json").write_text(log, encoding="utf-8")
        raise ValueError("Notarization failed or was not Accepted; see notary diagnostics and verify the runner profile")
    # Neither ZIP archives nor standalone Mach-O executables can be stapled.
