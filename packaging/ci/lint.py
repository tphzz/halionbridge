"""Run actionlint with a narrow compatibility exception for concurrency queues."""
import subprocess
from pathlib import Path


def check_queue_values(text):
    # actionlint 1.7.12 predates GitHub's queue property. Do not suppress other
    # concurrency errors; validate the only supported value here until upstream
    # understands it. GitHub accepts max only with cancel-in-progress: false.
    for line in text.splitlines():
        if line.lstrip().startswith("queue:") and line.strip() != "queue: max":
            raise ValueError("Concurrency queues must use queue: max")
        if line.lstrip().startswith("cancel-in-progress:") and line.strip() != "cancel-in-progress: false":
            raise ValueError("Queued builds must not cancel work in progress")


if __name__ == "__main__":
    for workflow in Path(".github/workflows").glob("*.yml"):
        check_queue_values(workflow.read_text(encoding="utf-8"))
    raise SystemExit(subprocess.call([
        "actionlint", "-shellcheck=", "-ignore",
        '^unexpected key "queue" for "concurrency" section\\. expected one of "cancel-in-progress", "group"$',
    ]))
