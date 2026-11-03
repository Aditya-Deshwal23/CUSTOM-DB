#!/usr/bin/env python3
"""Check least-privilege workflow defaults, per-job timeouts, and action pins."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORKFLOWS = (
    ".github/workflows/ci.yml",
    ".github/workflows/nightly.yml",
    ".github/workflows/release.yml",
    ".github/workflows/codeql.yml",
    ".github/workflows/scorecard.yml",
)
ACTION_PIN = re.compile(
    r"^\s*(?:-\s*)?uses:\s+[^@\s]+@([0-9a-f]{40})\s+#\s+v[0-9]+\.[0-9]+\.[0-9]+\s*$"
)
JOB_HEADER = re.compile(r"^  ([a-zA-Z0-9_-]+):\s*$")


def fail(message: str) -> None:
    print(f"workflow policy check: {message}", file=sys.stderr)
    raise SystemExit(1)


for relative in WORKFLOWS:
    path = ROOT / relative
    text = path.read_text(encoding="utf-8")
    if not re.search(r"^permissions:\n  contents: read$", text, re.MULTILINE):
        fail(f"{relative} must default to contents: read")
    if not re.search(r"^concurrency:\n", text, re.MULTILINE):
        fail(f"{relative} must define a concurrency group")

    in_jobs = False
    current_job: str | None = None
    current_lines: list[str] = []

    def finish_job() -> None:
        if current_job is not None and not any(
            re.match(r"^    timeout-minutes:\s*[1-9][0-9]*\s*$", line)
            for line in current_lines
        ):
            fail(f"{relative} job {current_job} has no timeout-minutes")

    for line in text.splitlines():
        if line == "jobs:":
            in_jobs = True
            continue
        if not in_jobs:
            continue
        header = JOB_HEADER.match(line)
        if header is not None:
            finish_job()
            current_job = header.group(1)
            current_lines = []
            continue
        if current_job is not None:
            current_lines.append(line)
        if "uses:" in line and ACTION_PIN.match(line) is None:
            fail(f"{relative} has an unpinned or unannotated action: {line.strip()}")
    finish_job()
    if not in_jobs or current_job is None:
        fail(f"{relative} has no jobs")

print(f"workflow policy check: validated {len(WORKFLOWS)} workflows")
