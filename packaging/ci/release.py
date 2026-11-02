"""Portable package verification and ref-owned draft releases (Python 3.11+).

Only the explicit `draft` command changes GitHub state. Builds and tests use
the offline functions; no code path publishes a release or changes a Git tag.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
import zipfile
from pathlib import Path, PurePosixPath
from urllib.parse import quote

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
MARKER = "halionbridge-ci-v1"


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def context() -> dict:
    ref = os.environ["GITHUB_REF"]
    if not ref.startswith(("refs/heads/", "refs/tags/")):
        raise ValueError("Only manually selected branches and tags are supported")
    sha = os.environ["GITHUB_SHA"]
    if not re.fullmatch(r"[0-9a-f]{40}", sha):
        raise ValueError("Invalid source SHA")
    configuration = os.environ["CONFIGURATION"]
    if configuration not in ("Release", "Debug"):
        raise ValueError("Invalid configuration")
    return dict(repository=os.environ["GITHUB_REPOSITORY"], ref=ref, sha=sha,
                configuration=configuration,
                **{key.lower().removeprefix("github_"): int(os.environ[key])
                   for key in ("GITHUB_RUN_ID", "GITHUB_RUN_NUMBER", "GITHUB_RUN_ATTEMPT")})


def target(ctx: dict) -> str:
    if ctx["configuration"] != "Release":
        raise ValueError("Debug builds are artifacts-only")
    ref = ctx["ref"]
    if ref.startswith("refs/tags/"):
        return ref.removeprefix("refs/tags/")
    if not ref.startswith("refs/heads/"):
        raise ValueError("Unsupported source ref")
    branch = ref.removeprefix("refs/heads/")
    slug = re.sub(r"[^A-Za-z0-9._-]+", "-", branch).strip("-.")[:64] or "branch"
    return f"preview-{slug}-{hashlib.sha256(branch.encode()).hexdigest()[:12]}"


def notes(ctx: dict, *, complete: bool = True) -> str:
    return (f"halionbridge {ctx['configuration']} build of `{ctx['ref']}` at `{ctx['sha']}`.\n\n"
            f"[Workflow run](https://github.com/{ctx['repository']}/actions/runs/{ctx['run_id']}).\n\n"
            f"{'Ready for manual review; never published automatically.' if complete else 'Asset update in progress; do not publish.'}\n\n"
            f"<!-- {MARKER} {json.dumps(ctx, sort_keys=True)} -->")


def check_existing(existing: dict, ctx: dict) -> None:
    if existing.get("draft") is not True:
        raise ValueError("Refusing to modify a published release")
    matches = re.findall(r"<!-- " + MARKER + r" (.*?) -->", existing.get("body") or "")
    if len(matches) != 1:
        raise ValueError("Refusing an unowned or ambiguous draft")
    owner = json.loads(matches[0])
    if not isinstance(owner, dict):
        raise ValueError("Malformed draft ownership marker")
    for key in ("repository", "ref", "configuration"):
        if owner.get(key) != ctx[key]:
            raise ValueError("Draft belongs to a different source ref or configuration")
    if any(type(owner.get(key)) is not int or owner[key] < 1 for key in ("run_number", "run_attempt")):
        raise ValueError("Malformed draft run identity")
    if (owner["run_number"], owner["run_attempt"]) > (ctx["run_number"], ctx["run_attempt"]):
        raise ValueError("Refusing to replace a newer draft with a stale run")


def verify_zip(path: Path, platform: str) -> None:
    binary = "halionbridge.exe" if platform.startswith("windows") else "halionbridge"
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)) or archive.testzip() is not None:
            raise ValueError("Duplicate or corrupt ZIP entries")
        if not set((binary, *REQUIRED_NOTICES)).issubset(names):
            raise ValueError("ZIP is missing CLI or notices")
        if set(DXSYX_NOTICES).intersection(names) and not set(DXSYX_NOTICES).issubset(names):
            raise ValueError("ZIP has incomplete dxsyx notices")
        if not any(n.startswith("examples/") and n.endswith("/halionbridge_build.lua") for n in names):
            raise ValueError("ZIP is missing build examples")
        for name in names:
            parts = PurePosixPath(name)
            if parts.is_absolute() or ".." in parts.parts or "\\" in name or ":" in name:
                raise ValueError("Unsafe ZIP path")
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


def verify_assets(root: Path, ctx: dict) -> list[Path]:
    result = []
    for platform in PLATFORMS:
        manifest_file = root / f"{platform}.json"
        if not manifest_file.is_file():
            raise ValueError(f"Missing platform manifest: {platform}")
        manifest = json.loads(manifest_file.read_text(encoding="utf-8"))
        for key, value in dict(ctx, platform=platform).items():
            if manifest.get(key) != value:
                raise ValueError(f"Mismatched {key} in {platform}")
        name = manifest["archive"]
        if not re.fullmatch(r"[A-Za-z0-9._-]+\.zip", name):
            raise ValueError("Unsafe archive filename")
        archive = root / name
        if not archive.is_file() or archive.is_symlink() or digest(archive) != manifest["sha256"]:
            raise ValueError(f"Archive checksum mismatch: {name}")
        verify_zip(archive, platform)
        result.append(archive)
    if len(set(result)) != len(PLATFORMS) or set(root.glob("*.zip")) != set(result):
        raise ValueError("Unexpected or duplicate platform archives")
    return result


def api(endpoint: str, method: str = "GET", payload: dict | None = None):
    command = ["gh", "api", endpoint, "--method", method]
    if payload is not None:
        command += ["--input", "-"]
    completed = subprocess.run(command, input=json.dumps(payload) if payload is not None else None,
                               text=True, capture_output=True, check=True)
    return json.loads(completed.stdout) if completed.stdout.strip() else None


def api_pages(endpoint: str) -> list:
    result = []
    page = 1
    while True:
        batch = api(f"{endpoint}?per_page=100&page={page}")
        if not isinstance(batch, list):
            raise ValueError("Expected a paginated GitHub response")
        result.extend(batch)
        if len(batch) < 100:
            return result
        page += 1


def verify_remote_tag(base: str, tag: str, ctx: dict) -> None:
    # Authenticated API reads work even when checkout disables Git credentials.
    # Resolve annotated tags to commits, without creating or moving any ref.
    refs = api(f"{base}/git/matching-refs/tags/{quote(tag, safe='')}")
    matches = [ref for ref in refs if ref["ref"] == f"refs/tags/{tag}"]
    if not matches and ctx["ref"].startswith("refs/heads/"):
        return
    if len(matches) != 1:
        raise ValueError("Source tag is missing or ambiguous")
    obj = matches[0]["object"]
    for _ in range(10):
        if obj["type"] == "commit":
            if obj["sha"] != ctx["sha"]:
                raise ValueError("Existing Git tag points elsewhere; refusing to move it")
            return
        if obj["type"] != "tag" or not re.fullmatch(r"[0-9a-f]{40}", obj["sha"]):
            break
        obj = api(f"{base}/git/tags/{obj['sha']}")["object"]
    raise ValueError("Git tag does not resolve to the dispatch commit")


def draft(root: Path, ctx: dict) -> None:
    if os.environ.get("GITHUB_EVENT_NAME") != "workflow_dispatch" or os.environ.get("GITHUB_WORKFLOW") != "00 Full - All Platforms":
        raise ValueError("Only a manual All Platforms run may update drafts")
    tag = target(ctx)
    archives = verify_assets(root, ctx)
    checksums = root / "SHA256SUMS.txt"
    checksums.write_text("".join(f"{digest(p)}  {p.name}\n" for p in archives), encoding="utf-8")
    files = archives + [root / f"{p}.json" for p in PLATFORMS] + [checksums]
    base = f"repos/{ctx['repository']}"
    # Enumerate drafts as well as published releases, and fail on API errors;
    # authentication failures must never be mistaken for an absent release.
    releases = [r for r in api_pages(f"{base}/releases") if r["tag_name"] == tag]
    if len(releases) > 1:
        raise ValueError("Multiple releases claim this target")
    if releases:
        check_existing(releases[0], ctx)
    verify_remote_tag(base, tag, ctx)
    metadata = dict(tag_name=tag, target_commitish=ctx["sha"], name=f"halionbridge {tag}",
                    body=notes(ctx, complete=False), draft=True,
                    prerelease=ctx["ref"].startswith("refs/heads/") or bool(re.search(r"\d-", tag)), make_latest="false")
    if releases:
        current = releases[0]
    else:
        current = api(f"{base}/releases", "POST", metadata)
    endpoint = f"{base}/releases/{current['id']}"
    check_existing(api(endpoint), ctx)
    api(endpoint, "PATCH", metadata)
    prefix = f"pending-{ctx['run_id']}-{ctx['run_attempt']}-"
    uploaded = []
    with tempfile.TemporaryDirectory(prefix="halionbridge-release-") as temporary:
        for path in files:
            check_existing(api(endpoint), ctx)
            pending_name = prefix + path.name
            assets = api_pages(endpoint + "/assets")
            for asset in assets:
                if asset["name"] == pending_name:
                    api(f"{base}/releases/assets/{asset['id']}", "DELETE")
            staged = Path(temporary) / pending_name
            shutil.copyfile(path, staged)
            subprocess.run(["gh", "release", "upload", tag, str(staged), "--repo", ctx["repository"]], check=True)
            matches = [a for a in api_pages(endpoint + "/assets") if a["name"] == pending_name]
            if len(matches) != 1 or matches[0].get("digest") != "sha256:" + digest(path):
                raise ValueError(f"Uploaded asset verification failed: {path.name}")
            uploaded.append((matches[0]["id"], path.name))
    # Old assets survive until every new upload has been verified. A failed
    # rename remains an unpublished, recoverable draft; a rerun repairs it.
    for asset in api_pages(endpoint + "/assets"):
        if asset["id"] not in {i for i, _ in uploaded}:
            check_existing(api(endpoint), ctx)
            api(f"{base}/releases/assets/{asset['id']}", "DELETE")
    for asset_id, name in uploaded:
        check_existing(api(endpoint), ctx)
        api(f"{base}/releases/assets/{asset_id}", "PATCH", {"name": name})
    check_existing(api(endpoint), ctx)
    api(endpoint, "PATCH", {"body": notes(ctx), "draft": True, "make_latest": "false"})


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("verify", "draft"))
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    try:
        (draft if args.command == "draft" else verify_assets)(args.directory, context())
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError, zipfile.BadZipFile) as error:
        parser.exit(1, f"Release processing failed: {error}\n")
