"""Stateful offline GitHub model with failure injection and no network access."""
import copy
import hashlib
from urllib.parse import parse_qs, urlsplit

import release


class Backend:
    def __init__(self, ctx, existing=True):
        self.ctx = ctx
        self.current = dict(id=1, tag_name=release.target(ctx), target_commitish=ctx["sha"],
                            draft=True, prerelease=ctx["ref"].startswith("refs/heads/"),
                            name="halionbridge " + release.target(ctx), body=release.notes(dict(ctx, run_number=1)),
                            html_url="https://github.com/owner/repo/releases/tag/actual-draft-url",
                            upload_url="https://uploads.github.com/repos/owner/repo/releases/1/assets{?name,label}") if existing else None
        self.others = [dict(id=2, tag_name="unrelated", name="unrelated", body="manual notes", draft=False,
                            html_url="https://github.com/owner/repo/releases/tag/unrelated")]
        self.assets = [dict(id=10, name="old.zip", state="uploaded", size=3,
                            digest="sha256:" + hashlib.sha256(b"old").hexdigest())] if existing else []
        self.refs = [dict(ref=ctx["ref"], object=dict(type="commit", sha=ctx["sha"]))] if ctx["ref"].startswith("refs/tags/") else []
        self.tag_objects = {}
        self.calls = []
        self.mutations = []
        self.failures = {}
        self.counts = {}
        self.next_id = 20
        self.upload_checks = []

    def fail(self, stage, occurrence=1):
        self.failures[stage] = self.counts.get(stage, 0) + occurrence

    def tick(self, stage):
        self.counts[stage] = self.counts.get(stage, 0) + 1
        if self.counts[stage] == self.failures.get(stage):
            del self.failures[stage]
            raise ValueError("simulated " + stage + " failure")

    def api(self, endpoint, method="GET", payload=None, *, binary=None):
        self.calls.append((endpoint, method))
        parsed = urlsplit(endpoint)
        path = parsed.path
        if not path.startswith("/"):
            path = "/" + path
        data = copy.deepcopy(payload)
        if method != "GET":
            self.mutations.append((endpoint, method, data))
            if "/git/" in path or (data and data.get("draft") is False):
                raise AssertionError("Git refs and publication must never be mutated")
        if "/git/matching-refs/" in path:
            return copy.deepcopy(self.refs)
        if "/git/tags/" in path:
            return copy.deepcopy(self.tag_objects[path.rsplit("/", 1)[1]])
        if path.endswith("/releases"):
            if method == "POST":
                self.tick("create")
                template = Backend(self.ctx).current
                self.current = dict(template, **data)
                self.assets = []
                self.tick("create-response")
                return copy.deepcopy(self.current)
            rows = ([self.current] if self.current else []) + self.others
            return self.page(rows, parsed.query)
        if path.endswith("/releases/1/assets"):
            if binary is not None:
                self.upload_checks.append([asset["name"] for asset in self.assets])
                name = parse_qs(parsed.query)["name"][0]
                content = binary.read_bytes()
                asset = dict(id=self.next_id, name=name, state="uploaded", size=len(content),
                             digest="sha256:" + hashlib.sha256(content).hexdigest())
                self.next_id += 1
                try:
                    self.tick("upload")
                except ValueError:
                    self.assets.append(dict(asset, state="starter", size=0, digest=None))
                    raise
                self.assets.append(asset)
                self.tick("upload-response")
                return copy.deepcopy(asset)
            self.tick("inventory")
            return self.page(self.assets, parsed.query)
        if path.endswith("/releases/1"):
            if method == "DELETE":
                self.tick("cleanup")
                self.current = None
                self.assets = []
                return None
            if method == "PATCH":
                stage = "complete" if '"state": "complete"' in data.get("body", "") else "metadata"
                self.tick(stage)
                self.current.update(data)
                self.tick(stage + "-response")
            return copy.deepcopy(self.current)
        if "/releases/assets/" in path:
            asset_id = int(path.rsplit("/", 1)[1])
            asset = next(asset for asset in self.assets if asset["id"] == asset_id)
            if method == "DELETE":
                self.tick("delete")
                self.assets.remove(asset)
                self.tick("delete-response")
            elif method == "PATCH":
                self.tick("rename")
                asset.update(data)
                self.tick("rename-response")
            return copy.deepcopy(asset)
        raise AssertionError(f"Unexpected {method} {endpoint}")

    @staticmethod
    def page(rows, query):
        page = int(parse_qs(query).get("page", ["1"])[0])
        return copy.deepcopy(rows[(page - 1) * 100:page * 100])
