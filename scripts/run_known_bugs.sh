#!/usr/bin/env bash
set -uo pipefail

test_binary="${1:-build/reldb_regression_tests}"
if [[ ! -x "$test_binary" ]]; then
    echo "Regression test binary not found or not executable: $test_binary" >&2
    exit 2
fi

work_dir="$(mktemp -d)"
xml_file="$work_dir/results.xml"
output_file="$work_dir/gtest.log"
cleanup() {
    rm -f "$xml_file" "$output_file"
    rmdir "$work_dir"
}
trap cleanup EXIT
args=(--gtest_also_run_disabled_tests "--gtest_output=xml:$xml_file")
if [[ -n "${GTEST_FILTER:-}" ]]; then
    args+=("--gtest_filter=$GTEST_FILTER")
fi

test_status=0
"$test_binary" "${args[@]}" >"$output_file" 2>&1 || test_status=$?
cat "$output_file"

python3 - "$xml_file" "$output_file" "$test_status" <<'PY'
import sys
import xml.etree.ElementTree as ET

xml_path, log_path, exit_status = sys.argv[1:]
expected = [
    "PersistenceTest.RestartKeepsData",
    "PersistenceTest.ExecutorIndexAndRowsSurviveCleanRestart",
    "DurabilityTest.DISABLED_HardKillKeepsAckedWrites",
    "BPlusTreeTest.LookupsNeverFalseMissAfterManyInserts",
    "BPlusTreeTest.RangeScanTerminatesAfterSplit",
    "BPlusTreeFuzzTest.MillionOperationsAgainstMapWithConstantEviction",
    "BPlusTreeTest.AscendingAndRandomFillFactorsBeatHalfFullBaseline",
    "BPlusTreeTest.DescendingAndHotKeyChurnRemainValidAfterDeletes",
    "WalTest.DISABLED_RecordAppendedAfterTornTailIsRecoverable",
    "ConcurrencyTest.ExecutorStatementGateTsanClean",
    "SqlTest.DISABLED_DocumentedExamplesWork",
    "MetricsTest.NoFabricatedValues",
]

results = {}
with open(log_path, encoding="utf-8", errors="replace") as log_file:
    log = log_file.read()
try:
    root = ET.parse(xml_path).getroot()
    for suite in root.findall(".//testsuite"):
        for case in suite.findall("testcase"):
            name = f"{suite.get('name')}.{case.get('name')}"
            failure = case.find("failure")
            skipped = case.find("skipped")
            if case.get("status") == "notrun":
                continue
            if skipped is not None:
                summary = skipped.get("message", "platform-specific test").replace("\n", " ")
                results[name] = ("SKIP", summary)
            elif failure is None:
                results[name] = ("PASS", "passed")
            else:
                lines = [line.strip() for line in failure.get("message", "").splitlines() if line.strip()]
                results[name] = ("FAIL", " | ".join(lines[-3:])[:180] or "assertion failed")
except (ET.ParseError, OSError):
    for name in expected:
        if f"[ RUN      ] {name}" in log or f"[ RUN      ] {name.replace('DISABLED_', '')}" in log:
            results[name] = ("ABORTED", "test process stopped before XML results were written")

if "ThreadSanitizer: data race" in log:
    summary = next(
        (line.strip() for line in log.splitlines() if line.startswith("SUMMARY: ThreadSanitizer:")),
        "ThreadSanitizer detected a data race",
    )
    results["ConcurrencyTest.ExecutorStatementGateTsanClean"] = ("FAIL", summary)

print("\nKnown regression results")
print("| Test | Status | Detail |")
print("|---|---|---|")
for name in expected:
    status, detail = results.get(name, ("NOT RUN", "not present in this invocation"))
    print(f"| {name} | {status} | {detail.replace('|', '/')} |")
print(f"\nGoogleTest exit status: {exit_status} (failures are expected until their fix phases).")
PY
