"""Offline portable ZIP packaging and complete release inventory validation."""
import hashlib
import json
import re
import stat
import subprocess
import zipfile
from pathlib import Path, PurePosixPath

PLATFORMS = ("windows-x64", "windows-arm64", "macos-universal")

NOTICES = {
    "LICENSE": "LICENSE",
    "THIRD_PARTY_NOTICES.md": "THIRD_PARTY_NOTICES.md",
    "licenses/JUCE-LICENSE.md": "external/JUCE/LICENSE.md",
    "licenses/spdlog-LICENSE.txt": "external/spdlog/LICENSE",
    "licenses/CLI11-LICENSE.txt": "external/CLI11/LICENSE",
    "licenses/sfizz-LICENSE.txt": "external/sfizz/LICENSE",
    "licenses/Apache-2.0.txt": "external/sfizz/external/abseil-cpp/LICENSE",
}

DXSYX_NOTICES = {
    "licenses/dxsyx/LICENSE.txt": "forks/dxsyx/LICENSE.txt",
    "licenses/dxsyx/UPSTREAM.md": "forks/dxsyx/UPSTREAM.md",
}

REQUIRED_NOTICES = tuple(NOTICES)


def digest(path: Path) -> str:
    checksum = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            checksum.update(block)
    return checksum.hexdigest()


def verify_zip(path: Path, platform: str) -> None:
    if platform not in PLATFORMS:
        raise ValueError("Unknown package platform")
    binary = "halionbridge.exe" if platform.startswith("windows") else "halionbridge"
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        if len(names) != len({name.casefold() for name in names}) or archive.testzip() is not None:
            raise ValueError("Duplicate or corrupt ZIP entries")
        if not set((binary, *REQUIRED_NOTICES)).issubset(names):
            raise ValueError("ZIP is missing CLI or notices")
        if set(DXSYX_NOTICES).intersection(names) and not set(DXSYX_NOTICES).issubset(names):
            raise ValueError("ZIP has incomplete dxsyx notices")
        if not any(n.startswith("examples/") and n.endswith("/halionbridge_build.lua") for n in names):
            raise ValueError("ZIP is missing build examples")
        for member in archive.infolist():
            name = member.filename
            parts = PurePosixPath(name)
            if parts.is_absolute() or parts.as_posix() != name or ".." in parts.parts or "\\" in name or ":" in name:
                raise ValueError("Unsafe ZIP path")
            kind = stat.S_IFMT(member.external_attr >> 16)
            if member.is_dir() or kind not in (0, stat.S_IFREG) or member.flag_bits & 1:
                raise ValueError("ZIP entries must be unencrypted regular files")
            if (name in (binary, *REQUIRED_NOTICES, *DXSYX_NOTICES) or name.endswith("/halionbridge_build.lua")) and member.file_size == 0:
                raise ValueError(f"Empty required ZIP entry: {name}")
            if name not in (binary, *REQUIRED_NOTICES, *DXSYX_NOTICES) and not name.startswith("examples/"):
                raise ValueError(f"Unexpected ZIP content: {name}")
            if name.startswith("examples/") and (parts.suffix.lower() in (".dll", ".lib", ".pdb", ".vstpreset", ".dylib", ".a", ".exe", ".exp")
                                                or any(p == "build" or p.startswith("build-") for p in parts.parts)):
                raise ValueError(f"Build output in examples: {name}")


def package(source: Path, binary: Path, output: Path, basename: str, platform: str, ctx: dict) -> Path:
    if platform not in PLATFORMS or not re.fullmatch(r"halionbridge-[A-Za-z0-9._-]+", basename):
        raise ValueError("Invalid package identity")
    tag = ctx["ref"].removeprefix("refs/tags/")
    if ctx["ref"].startswith("refs/tags/") and re.fullmatch(r"v?\d+(\.\d+){1,2}([-.][0-9A-Za-z.]+)?", tag):
        basename = "halionbridge-" + tag.removeprefix("v")
    suffix = "-debug" if ctx["configuration"] == "Debug" else ""
    output.mkdir(parents=True, exist_ok=True)
    result = output / f"{basename}-{platform}{suffix}.zip"
    examples = subprocess.check_output(["git", "ls-files", "-z", "examples"], cwd=source).decode().split("\0")
    # Older source refs do not contain the DX7 converter. Keep their packages
    # usable while requiring both notices whenever that dependency is present.
    notices = dict(NOTICES)
    if (source / "forks/dxsyx").exists():
        notices.update(DXSYX_NOTICES)
    entries = {**{name: source / origin for name, origin in notices.items()}, binary.name: binary}
    entries.update({name: source / name for name in examples if name})
    with zipfile.ZipFile(result, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, path in sorted(entries.items()):
            if not path.is_file() or path.is_symlink():
                raise ValueError(f"Missing or symlinked package input: {name}")
            archive.write(path, name)
    verify_zip(result, platform)
    return result


def write_manifest(archive: Path, platform: str, ctx: dict) -> None:
    manifest = dict(ctx, platform=platform, archive=archive.name, sha256=digest(archive))
    (archive.parent / f"{platform}.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    (archive.parent / f"{platform}.sha256").write_text(f"{manifest['sha256']}  {archive.name}\n", encoding="utf-8")


def require_file(path: Path, *, nonempty=True) -> None:
    if path.is_symlink() or not path.is_file() or (nonempty and path.stat().st_size == 0):
        raise ValueError(f"Missing, empty or non-regular package file: {path.name}")


def json_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def validate_archive_name(name: str, platform: str, ctx: dict) -> None:
    if not isinstance(name, str):
        raise ValueError("Invalid archive filename")
    ref = ctx["ref"].split("/", 2)[2]
    suffix = "-debug" if ctx["configuration"] == "Debug" else ""
    if ctx["ref"].startswith("refs/tags/") and re.fullmatch(r"v?\d+(\.\d+){1,2}([-.][0-9A-Za-z.]+)?", ref):
        if name != f"halionbridge-{ref.removeprefix('v')}-{platform}{suffix}.zip":
            raise ValueError("Unexpected tag package filename")
        return
    # Keep the exact CMake ref sanitization and Git's minimum-seven-digit SHA.
    sanitized = re.sub(r"-+", "-", re.sub(r"[^A-Za-z0-9._-]+", "-", ref)).strip("-") or "unknown"
    match = re.fullmatch(r"halionbridge-" + re.escape(sanitized) + r"-([0-9a-f]{7,40})-" +
                         re.escape(platform + suffix) + r"\.zip", name)
    if match is None or not ctx["sha"].startswith(match[1]):
        raise ValueError("Package filename does not match the source ref and commit")


def verify_assets(root: Path, ctx: dict) -> list[Path]:
    if root.is_symlink() or not root.is_dir():
        raise ValueError("Package input must be a real directory")
    result = []
    expected = set()
    for platform in PLATFORMS:
        manifest_file = root / f"{platform}.json"
        require_file(manifest_file)
        manifest = json.loads(manifest_file.read_text(encoding="utf-8"), object_pairs_hook=json_object)
        if not isinstance(manifest, dict):
            raise ValueError(f"Expected a JSON object in {platform}")
        if set(manifest) != set(ctx) | {"platform", "archive", "sha256"}:
            raise ValueError(f"Unexpected manifest fields in {platform}")
        for key, value in dict(ctx, platform=platform).items():
            if type(manifest.get(key)) is not type(value) or manifest[key] != value:
                raise ValueError(f"Mismatched {key} in {platform}")
        name = manifest.get("archive")
        validate_archive_name(name, platform, ctx)
        archive = root / name
        require_file(archive)
        checksum = manifest.get("sha256")
        if not isinstance(checksum, str) or not re.fullmatch(r"[0-9a-f]{64}", checksum) or digest(archive) != checksum:
            raise ValueError(f"Archive checksum mismatch: {name}")
        sidecar = root / f"{platform}.sha256"
        require_file(sidecar)
        if sidecar.read_text(encoding="utf-8").splitlines() != [f"{checksum}  {name}"]:
            raise ValueError(f"Invalid platform checksum: {platform}")
        verify_zip(archive, platform)
        expected.update((name, manifest_file.name, sidecar.name))
        result.append(archive)
    if (root / "SHA256SUMS.txt").exists() or (root / "SHA256SUMS.txt").is_symlink():
        require_file(root / "SHA256SUMS.txt", nonempty=False)
        expected.add("SHA256SUMS.txt")
    if len(set(result)) != len(PLATFORMS) or {path.name for path in root.iterdir()} != expected:
        raise ValueError("Unexpected or duplicate package inputs")
    return result
