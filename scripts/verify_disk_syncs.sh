#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 PATH_TO_DISK_SYNC_PROBE" >&2
    exit 2
fi

if ! command -v strace >/dev/null 2>&1; then
    echo "strace is required for the Linux disk-sync check" >&2
    exit 2
fi

temporary_directory="$(mktemp -d)"
trap 'rm -rf "$temporary_directory"' EXIT

strace -f -e trace=fdatasync,fsync -o "$temporary_directory/trace" \
    "$1" "$temporary_directory/database.db"

fdatasync_count="$(grep -c 'fdatasync(' "$temporary_directory/trace" || true)"
directory_fsync_count="$(grep -c 'fsync(' "$temporary_directory/trace" || true)"

if [[ "$fdatasync_count" -ne 2 ]]; then
    echo "expected 2 fdatasync calls (Open initialization and explicit Sync), got $fdatasync_count" >&2
    cat "$temporary_directory/trace" >&2
    exit 1
fi

if [[ "$directory_fsync_count" -ne 1 ]]; then
    echo "expected 1 parent-directory fsync for file creation, got $directory_fsync_count" >&2
    cat "$temporary_directory/trace" >&2
    exit 1
fi

echo "Disk sync trace passed: two fdatasync calls correspond to Open initialization and explicit Sync."
