#!/usr/bin/env python3
"""Run Lighthouse against the actual dashboard served by a local engine binary."""

import argparse
import datetime
import json
import os
import shutil
import socket
import subprocess
import tempfile
import time
import urllib.request


def available_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def wait_for_engine(process, base_url):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"Engine exited with status {process.returncode}")
        try:
            with urllib.request.urlopen(base_url + "/healthz", timeout=1) as response:
                if response.status == 200:
                    return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError("Engine did not become ready for Lighthouse")


def install_chrome_path(environment):
    if environment.get("CHROME_PATH"):
        return
    try:
        from playwright.sync_api import sync_playwright

        with sync_playwright() as playwright:
            environment["CHROME_PATH"] = playwright.chromium.executable_path
    except ImportError:
        return


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, help="Path to relational_db")
    parser.add_argument("--output", required=True, help="JSON report output path")
    args = parser.parse_args()
    npx = shutil.which("npx")
    if npx is None:
        raise RuntimeError("npx is required to run the Lighthouse CLI")

    port = available_port()
    with tempfile.TemporaryDirectory(prefix="custom-db-lighthouse-") as directory:
        database = os.path.join(directory, "lighthouse.db")
        base_url = f"http://127.0.0.1:{port}"
        environment = os.environ.copy()
        environment.update({"PORT": str(port), "DB_PATH": database})
        install_chrome_path(environment)
        process = subprocess.Popen(
            [os.path.abspath(args.binary)],
            cwd=os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            env=environment,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            wait_for_engine(process, base_url)
            version_request = urllib.request.Request(
                base_url + "/api/v1/version", headers={"Accept": "application/json"}
            )
            with urllib.request.urlopen(version_request, timeout=5) as response:
                version = json.loads(response.read())
            raw_report = os.path.join(directory, "lighthouse.json")
            command = [
                npx,
                "--yes",
                "lighthouse",
                base_url,
                "--only-categories=performance,accessibility",
                "--output=json",
                f"--output-path={raw_report}",
                "--chrome-flags=--headless --no-sandbox",
                "--quiet",
            ]
            result = subprocess.run(
                command,
                check=False,
                capture_output=True,
                text=True,
                timeout=240,
                env=environment,
            )
            if result.returncode != 0:
                raise RuntimeError(
                    f"Lighthouse failed with exit status {result.returncode}:\n{result.stderr}"
                )
            with open(raw_report, encoding="utf-8") as report_file:
                lighthouse = json.load(report_file)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)

    categories = lighthouse["categories"]
    scores = {
        name: round(category["score"] * 100)
        for name, category in categories.items()
        if name in {"performance", "accessibility"}
    }
    audit_ids = (
        "first-contentful-paint",
        "largest-contentful-paint",
        "speed-index",
        "total-blocking-time",
        "cumulative-layout-shift",
        "interactive",
        "color-contrast",
        "label",
        "button-name",
        "document-title",
    )
    metrics = {}
    for audit_id in audit_ids:
        audit = lighthouse["audits"].get(audit_id)
        if audit is not None:
            metrics[audit_id] = {
                "score": audit.get("score"),
                "display_value": audit.get("displayValue", ""),
            }
    summary = {
        "audited_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "lighthouse_version": lighthouse.get("lighthouseVersion"),
        "engine_build": version.get("git_sha", "unknown"),
        "scores_percent": scores,
        "metrics_and_accessibility_audits": metrics,
        "threshold_percent": 90,
    }
    output = os.path.abspath(args.output)
    os.makedirs(os.path.dirname(output), exist_ok=True)
    with open(output, "w", encoding="utf-8") as report_file:
        json.dump(summary, report_file, indent=2)
        report_file.write("\n")
    print(json.dumps(summary, indent=2))
    if any(score < 90 for score in scores.values()) or set(scores) != {
        "performance",
        "accessibility",
    }:
        raise SystemExit("Lighthouse performance and accessibility must both be at least 90.")


if __name__ == "__main__":
    main()
