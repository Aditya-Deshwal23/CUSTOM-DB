#!/usr/bin/env bash
set -euo pipefail

binary="${1:?usage: check_binary_hardening.sh <binary>}"
if [[ ! -x "${binary}" ]]; then
    echo "Expected an executable binary at ${binary}" >&2
    exit 1
fi

checksec_report="$(checksec --file="${binary}")"
printf '%s\n' "${checksec_report}"
for feature in "Full RELRO" "Canary found" "NX enabled" "PIE enabled"; do
    if ! grep -Fqi "${feature}" <<<"${checksec_report}"; then
        echo "Missing required binary hardening: ${feature}" >&2
        exit 1
    fi
done

hardening_report="$(hardening-check "${binary}" 2>&1 || true)"
printf '%s\n' "${hardening_report}"
if ! grep -Eiq 'Fortify Source functions:[[:space:]]+yes|Fortify Source functions:[[:space:]]+[1-9][0-9]*' <<<"${hardening_report}"; then
    echo "FORTIFY_SOURCE was not detected in the built binary" >&2
    exit 1
fi
