#!/usr/bin/env bash
set -euo pipefail

mode="${1:---check}"
shift || true
base="${FORMAT_BASE:-HEAD}"

if [[ "$mode" == "--check" ]]; then
    if (($#)); then
        git clang-format --extensions cpp,h,hpp --diff "$base" -- "$@"
    else
        git clang-format --extensions cpp,h,hpp --diff "$base"
    fi
elif [[ "$mode" == "--fix" ]]; then
    if (($#)); then
        git clang-format --extensions cpp,h,hpp --force "$base" -- "$@"
    else
        git clang-format --extensions cpp,h,hpp --force "$base"
    fi
else
    echo "Usage: $0 [--check|--fix] [files...]" >&2
    exit 2
fi
