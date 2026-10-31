#!/usr/bin/env python3
"""Reject fabricated dashboard metrics and scripted simulation traces."""

import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
DASHBOARD = ROOT / "dashboard"
FORBIDDEN = (
    "Math.random",
    "runTelemetrySimulation",
    "runSQLSimulation",
    "simulateBufferPoolActivity",
    "ILLUSTRATIVE_TRACE",
    "setTimeout",
)


def main():
    violations = []
    for path in DASHBOARD.rglob("*"):
        if not path.is_file() or path.suffix not in {".js", ".html", ".css"}:
            continue
        content = path.read_text(encoding="utf-8")
        for token in FORBIDDEN:
            if token in content:
                violations.append(f"{path.relative_to(ROOT)} contains forbidden token {token!r}")
    if violations:
        print("\n".join(violations), file=sys.stderr)
        return 1
    print("Dashboard honesty guard passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
