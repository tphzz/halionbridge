"""Portable package verification and ref-owned draft releases (Python 3.10+).

Only the explicit `draft` command changes GitHub state. Builds and tests use
the offline functions; no code path publishes a release or changes a Git tag.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import uuid
import zipfile
from pathlib import Path
from urllib.parse import quote, urlencode

from release_packages import (
    PLATFORMS, NOTICES, REQUIRED_NOTICES, digest, json_object,
    package, verify_assets, verify_zip, write_manifest,
)

MARKER = "halionbridge-ci-v1"


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
    repository = os.environ["GITHUB_REPOSITORY"]
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository) or not ref.split("/", 2)[2]:
        raise ValueError("Invalid repository or source ref")
    runs = {key.lower().removeprefix("github_"): int(os.environ[key])
            for key in ("GITHUB_RUN_ID", "GITHUB_RUN_NUMBER", "GITHUB_RUN_ATTEMPT")}
    if any(value < 1 for value in runs.values()):
        raise ValueError("Invalid workflow run identity")
    return dict(repository=repository, ref=ref, sha=sha,
                configuration=configuration,
                **runs)


def target(ctx: dict) -> str:
    if ctx["configuration"] != "Release":
        raise ValueError("Debug builds are artifacts-only")
    ref = ctx["ref"]
    if ref.startswith("refs/tags/"):
        tag = ref.removeprefix("refs/tags/")
        if tag == "snapshot" or tag.startswith("snapshot/"):
            raise ValueError("The snapshot/ namespace is reserved for branch drafts")
        return tag
    if not ref.startswith("refs/heads/"):
        raise ValueError("Unsupported source ref")
    return "snapshot/" + ref.removeprefix("refs/heads/")


def notes(ctx: dict, *, complete: bool = True) -> str:
    owner = dict(ctx, target_tag=target(ctx), state="complete" if complete else "incomplete")
    return (f"halionbridge {ctx['configuration']} build of `{ctx['ref']}` at `{ctx['sha']}`.\n\n"
            f"[Workflow run](https://github.com/{ctx['repository']}/actions/runs/{ctx['run_id']}).\n\n"
            f"{'Ready for manual review; never published automatically.' if complete else 'Asset update in progress; do not publish.'}\n\n"
            f"<!-- {MARKER} {json.dumps(owner, sort_keys=True)} -->")


def owners(existing: dict) -> list[dict]:
    """Read ownership hints without mistaking unrelated malformed notes for ownership."""
    body = existing.get("body") or ""
    if not isinstance(body, str):
        raise ValueError("Invalid release notes response")
    result = []
    for match in re.findall(r"<!-- " + MARKER + r" (.*?) -->", body):
        try:
            owner = json.loads(match, object_pairs_hook=json_object)
        except ValueError:
            continue
        if isinstance(owner, dict):
            result.append(owner)
    return result


def check_existing(existing: dict, ctx: dict) -> None:
    if not isinstance(existing, dict):
        raise ValueError("Malformed GitHub release response")
    if existing.get("draft") is not True:
        raise ValueError("Refusing to modify a published release")
    body = existing.get("body") or ""
    if not isinstance(body, str):
        raise ValueError("Malformed draft notes")
    matches = re.findall(r"<!-- " + MARKER + r" (.*?) -->", body)
    if len(matches) != 1:
        raise ValueError("Refusing an unowned or ambiguous draft")
    owner = json.loads(matches[0], object_pairs_hook=json_object)
    if not isinstance(owner, dict):
        raise ValueError("Malformed draft ownership marker")
    for key in ("repository", "ref", "configuration"):
        if owner.get(key) != ctx[key]:
            raise ValueError("Draft belongs to a different source ref or configuration")
    if any(type(owner.get(key)) is not int or owner[key] < 1 for key in ("run_id", "run_number", "run_attempt")):
        raise ValueError("Malformed draft run identity")
    if (owner["run_number"], owner["run_attempt"]) > (ctx["run_number"], ctx["run_attempt"]):
        raise ValueError("Refusing to replace a newer draft with a stale run")
    if not isinstance(owner.get("sha"), str) or not re.fullmatch(r"[0-9a-f]{40}", owner["sha"]):
        raise ValueError("Malformed draft source commit")
    if "target_tag" not in owner and "state" not in owner:
        if existing.get("tag_name") != target(ctx):
            raise ValueError("Legacy draft needs explicit reconciliation before preparing a snapshot")
    elif owner.get("target_tag") != target(ctx) or owner.get("state") not in ("complete", "incomplete"):
        raise ValueError("Draft has conflicting intended identity or completion state")


def find_release(releases: list, ctx: dict):
    tag = target(ctx)
    candidates = []
    branch = ctx["ref"].startswith("refs/heads/")
    raw_branch = ctx["ref"].removeprefix("refs/heads/")
    slug = re.sub(r"[^A-Za-z0-9._-]+", "-", raw_branch).strip("-.")[:64] or "branch"
    legacy = f"preview-{slug}-{hashlib.sha256(raw_branch.encode()).hexdigest()[:12]}"
    for item in releases:
        if type(item.get("id")) is not int or item["id"] < 1 or not isinstance(item.get("tag_name"), str):
            raise ValueError("Malformed GitHub release listing")
        claimed = any(owner.get("repository") == ctx["repository"] and owner.get("ref") == ctx["ref"]
                      for owner in owners(item))
        legacy_match = branch and (item["tag_name"] == legacy or item.get("name") == "halionbridge " + legacy)
        if item["tag_name"] == tag or claimed or legacy_match:
            candidates.append(item)
    if len(candidates) > 1:
        identities = ", ".join(str(item.get("html_url") or item["id"]) for item in candidates)
        raise ValueError("Multiple releases claim this target; explicit reconciliation required: " + identities)
    return candidates[0] if candidates else None


def api(endpoint: str, method: str = "GET", payload: dict | None = None, *, binary: Path | None = None):
    if not os.environ.get("GH_TOKEN", "").strip():
        raise ValueError("The release step requires a nonempty GH_TOKEN")
    command = ["gh", "api", endpoint, "--hostname", "github.com", "--method", method,
               "--header", "Accept: application/vnd.github+json"]
    if binary is not None:
        command += ["--header", "Content-Type: application/octet-stream", "--input", str(binary)]
    elif payload is not None:
        command += ["--input", "-"]
    try:
        completed = subprocess.run(command, input=json.dumps(payload) if payload is not None else None,
                                   text=True, capture_output=True, timeout=180)
    except subprocess.TimeoutExpired as error:
        raise ValueError(f"GitHub API {method} timed out; its outcome may be uncertain") from error
    if completed.returncode:
        status = re.search(r"HTTP (\d{3})", completed.stderr or "")
        detail = "HTTP " + status[1] if status else "exit " + str(completed.returncode)
        # Raw CLI diagnostics may contain unrelated environment or server data.
        raise ValueError(f"GitHub API {method} failed ({detail}); check repository access, release credential permissions and network")
    return json.loads(completed.stdout) if completed.stdout.strip() else None


def api_pages(endpoint: str) -> list:
    result = []
    page = 1
    while True:
        batch = api(f"{endpoint}?per_page=100&page={page}")
        if not isinstance(batch, list) or any(not isinstance(item, dict) for item in batch):
            raise ValueError("Expected a paginated GitHub response")
        result.extend(batch)
        if len(batch) < 100:
            return result
        page += 1


def verify_remote_tag(base: str, tag: str, ctx: dict) -> None:
    # Authenticated API reads work even when checkout disables Git credentials.
    # Resolve annotated tags to commits, without creating or moving any ref.
    branch = ctx["ref"].startswith("refs/heads/")
    refs = api(f"{base}/git/matching-refs/tags/" + ("" if branch else quote(tag, safe="")))
    if not isinstance(refs, list) or any(not isinstance(ref, dict) or not isinstance(ref.get("ref"), str) for ref in refs):
        raise ValueError("Malformed Git tag response")
    if branch:
        name = "refs/tags/" + tag
        if any(ref["ref"] == name or ref["ref"].startswith(name + "/") or name.startswith(ref["ref"] + "/") for ref in refs):
            raise ValueError("Existing snapshot Git ref requires explicit reconciliation; no refs were changed")
        return
    matches = [ref for ref in refs if ref["ref"] == f"refs/tags/{tag}"]
    if len(matches) != 1:
        raise ValueError("Source tag is missing or ambiguous")
    obj = matches[0].get("object")
    for _ in range(10):
        if not isinstance(obj, dict) or not isinstance(obj.get("sha"), str) or not re.fullmatch(r"[0-9a-f]{40}", obj["sha"]):
            raise ValueError("Malformed Git tag object")
        if obj.get("type") == "commit":
            if obj["sha"] != ctx["sha"]:
                raise ValueError("Existing Git tag points elsewhere; refusing to move it")
            return
        if obj.get("type") != "tag":
            break
        result = api(f"{base}/git/tags/{obj['sha']}")
        obj = result.get("object") if isinstance(result, dict) else None
    raise ValueError("Git tag does not resolve to the dispatch commit")


def release_url(current: dict, ctx: dict) -> str:
    url = current.get("html_url")
    if not isinstance(url, str) or not url.startswith(f"https://github.com/{ctx['repository']}/releases/") or any(c.isspace() for c in url):
        raise ValueError("Missing or invalid GitHub release URL")
    return url


def metadata(ctx: dict, *, complete=False) -> dict:
    tag = target(ctx)
    return dict(tag_name=tag, target_commitish=ctx["sha"], name=f"halionbridge {tag}",
                body=notes(ctx, complete=complete), draft=True,
                prerelease=ctx["ref"].startswith("refs/heads/") or bool(re.search(r"\d-", tag)), make_latest="false")


def checked_current(endpoint: str, ctx: dict, *, complete=False) -> dict:
    current = api(endpoint)
    if not isinstance(current, dict) or type(current.get("id")) is not int or current["id"] != int(endpoint.rsplit("/", 1)[1]):
        raise ValueError("Malformed GitHub release response")
    check_existing(current, ctx)
    for key, value in metadata(ctx, complete=complete).items():
        if key != "make_latest" and (type(current.get(key)) is not type(value) or current[key] != value):
            raise ValueError(f"Release metadata verification failed: {key}")
    release_url(current, ctx)
    return current


def asset_matches(asset: dict, expected: dict) -> bool:
    return (type(asset.get("id")) is int and asset["id"] > 0 and asset.get("state") == "uploaded"
            and type(asset.get("size")) is int and asset["size"] == expected["size"]
            and asset.get("digest") == expected["digest"])


def release_assets(endpoint: str) -> list:
    assets = api_pages(endpoint + "/assets")
    if (any(type(asset.get("id")) is not int or asset["id"] < 1 or not isinstance(asset.get("name"), str) for asset in assets)
            or len({asset["id"] for asset in assets}) != len(assets)):
        raise ValueError("Malformed release asset identity")
    return assets


def verify_inventory(endpoint: str, expected: dict) -> None:
    assets = release_assets(endpoint)
    if (len(assets) != len(expected) or {asset.get("name") for asset in assets} != set(expected)
            or len({asset.get("id") for asset in assets}) != len(expected)
            or any(not asset_matches(asset, expected[asset["name"]]) for asset in assets)):
        raise ValueError("Final release inventory verification failed")


def refresh_assets(endpoint: str, current: dict, files: list[Path], ctx: dict) -> None:
    expected = {path.name: dict(size=path.stat().st_size, digest="sha256:" + digest(path)) for path in files}
    raw_upload_url = current.get("upload_url")
    if not isinstance(raw_upload_url, str):
        raise ValueError("Missing GitHub upload URL")
    upload_url = raw_upload_url.split("{", 1)[0]
    if upload_url != f"https://uploads.github.com/repos/{ctx['repository']}/releases/{current['id']}/assets":
        raise ValueError("Unexpected GitHub upload URL")
    prefix = f"pending-{ctx['run_id']}-{ctx['run_attempt']}-{uuid.uuid4().hex}-"
    uploaded = {}
    for path in files:
        checked_current(endpoint, ctx)
        pending_name = prefix + path.name
        api(upload_url + "?" + urlencode(dict(name=pending_name)), "POST", binary=path)
        matches = [asset for asset in release_assets(endpoint) if asset["name"] == pending_name]
        if len(matches) != 1 or not asset_matches(matches[0], expected[path.name]):
            raise ValueError(f"Uploaded asset verification failed: {path.name}")
        uploaded[matches[0]["id"]] = path.name
    # Retain the previous complete set until all seven incoming uploads exist
    # and verify. A fresh staging prefix also makes identical-commit retries safe.
    assets = release_assets(endpoint)
    base = f"repos/{ctx['repository']}"
    for asset in assets:
        if asset["id"] not in uploaded:
            checked_current(endpoint, ctx)
            api(f"{base}/releases/assets/{asset['id']}", "DELETE")
    for asset_id, name in uploaded.items():
        checked_current(endpoint, ctx)
        api(f"{base}/releases/assets/{asset_id}", "PATCH", dict(name=name))
    checked_current(endpoint, ctx)
    verify_inventory(endpoint, expected)
    verify_remote_tag(base, target(ctx), ctx)
    api(endpoint, "PATCH", dict(body=notes(ctx), draft=True, make_latest="false"))
    checked_current(endpoint, ctx, complete=True)
    verify_inventory(endpoint, expected)


def cleanup_created(endpoint: str, ctx: dict) -> None:
    current = api(endpoint)
    if not isinstance(current, dict) or type(current.get("id")) is not int or current["id"] != int(endpoint.rsplit("/", 1)[1]):
        raise ValueError("Cannot confirm newly created draft identity")
    check_existing(current, ctx)
    owner = owners(current)[0]
    if any(owner.get(key) != value for key, value in ctx.items()) or owner.get("target_tag") != target(ctx):
        raise ValueError("Newly created draft ownership changed; refusing cleanup")
    api(endpoint, "DELETE")


def draft(root: Path, ctx: dict) -> dict:
    if os.environ.get("GITHUB_EVENT_NAME") != "workflow_dispatch" or os.environ.get("GITHUB_WORKFLOW") != "00 Full - All Platforms":
        raise ValueError("Only a manual All Platforms run may update drafts")
    if not os.environ.get("GH_TOKEN", "").strip():
        raise ValueError("The release step requires a nonempty GH_TOKEN")
    tag = target(ctx)
    base = f"repos/{ctx['repository']}"
    current = find_release(api_pages(base + "/releases"), ctx)
    is_tag = ctx["ref"].startswith("refs/tags/")
    if current is not None and is_tag:
        return dict(action="preserved", url=release_url(current, ctx))
    if current is not None:
        check_existing(current, ctx)
    verify_remote_tag(base, tag, ctx)
    archives = verify_assets(root, ctx)
    payloads = sorted(archives + [root / f"{platform}.json" for platform in PLATFORMS])
    checksums = root / "SHA256SUMS.txt"
    checksums.write_text("".join(f"{digest(path)}  {path.name}\n" for path in payloads), encoding="utf-8")
    created = current is None
    if created:
        try:
            current = api(base + "/releases", "POST", metadata(ctx))
        except (ValueError, OSError, subprocess.SubprocessError) as error:
            raise ValueError(f"Draft creation outcome is uncertain: {error}. Manual recovery: inspect drafts for {tag!r} and run {ctx['run_id']}; do not blindly create another draft or change its Git tag") from error
        if not isinstance(current, dict) or type(current.get("id")) is not int or current["id"] < 1:
            raise ValueError(f"Draft creation outcome is uncertain for {tag!r}; inspect drafts before retrying")
    endpoint = f"{base}/releases/{current['id']}"
    try:
        if not created:
            check_existing(api(endpoint), ctx)
            api(endpoint, "PATCH", metadata(ctx))
        current = checked_current(endpoint, ctx)
        refresh_assets(endpoint, current, payloads + [checksums], ctx)
    except (ValueError, OSError, subprocess.SubprocessError, KeyboardInterrupt) as error:
        location = current.get("html_url") or f"release ID {current['id']}"
        if created and is_tag:
            try:
                cleanup_created(endpoint, ctx)
            except (ValueError, OSError, subprocess.SubprocessError) as cleanup_error:
                raise ValueError(f"Tag draft preparation failed: {error}. Manual recovery: inspect {location} (ID {current['id']}), confirm it is an owned unpublished draft, delete only that draft by ID and retry; preserve its Git tag. Cleanup could not be confirmed: {cleanup_error}") from error
            raise ValueError(f"Tag preparation failed: {error}; removed only the newly created draft (ID {current['id']}); its Git tag is preserved. Retry all jobs") from error
        # If finalization succeeded server-side but its response was lost, put
        # this invocation's draft back into incomplete state when ownership holds.
        try:
            actual = api(endpoint)
            check_existing(actual, ctx)
            if all(owners(actual)[0].get(key) == value for key, value in ctx.items()):
                api(endpoint, "PATCH", dict(body=notes(ctx, complete=False), draft=True, make_latest="false"))
        except (ValueError, OSError, subprocess.SubprocessError):
            pass
        raise ValueError(f"Draft refresh not confirmed complete: {error}. Inspect {location}; do not publish it until a successful retry. Re-run all jobs to repair the draft") from error
    return dict(action="created" if created else "refreshed", url=release_url(current, ctx))


def report(result: dict) -> None:
    message = f"Release {result['action']}: {result['url']}"
    print(message, flush=True)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with Path(os.environ["GITHUB_STEP_SUMMARY"]).open("a", encoding="utf-8") as stream:
            stream.write(message + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("verify", "draft"))
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    try:
        ctx = context()
        if args.command == "draft":
            report(draft(args.directory, ctx))
        else:
            verify_assets(args.directory, ctx)
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError, zipfile.BadZipFile) as error:
        parser.exit(1, f"Release processing failed: {error}\n")
