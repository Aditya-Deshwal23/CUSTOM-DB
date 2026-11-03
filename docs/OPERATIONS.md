# Operations runbook

CUSTOM-DB is an educational, single-process database. This runbook is for
local or demonstration deployments, not a production availability promise.
Never use the public Render demo for durable or sensitive data.

## Start and stop

```sh
docker compose up -d --build
curl -fsS http://localhost:8080/healthz
curl -fsS http://localhost:8080/readyz
docker compose logs --follow custom-db
docker compose stop
docker compose start
```

`/healthz` reports process liveness; `/readyz` reports whether recovery has
completed. A readiness failure should be treated as an unavailable service.
Use `docker compose down` to remove the container while retaining the named
data volume. Do not remove the volume unless intentionally deleting data.

## Configuration and access

Review [CONFIG.md](CONFIG.md) before deployment. Set `API_KEY` using the
deployment's secret manager when exposing write/admin routes, restrict network
access, and keep `CORS_ORIGINS` narrow. The Compose file uses a read-only root,
non-root UID, dropped capabilities, and explicit process/resource limits.
Configuration is logged with secrets redacted; still treat logs as operational
data.

## Backup and restore

This version has no online backup/restore command. A consistent file-level
copy requires stopping the service cleanly and preserving both the database
and its WAL from the same stopped state:

```sh
mkdir -p backups
docker compose stop
container_id="$(docker compose ps --all --quiet custom-db)"
data_volume="$(docker inspect "${container_id}" \
  --format '{{range .Mounts}}{{if eq .Destination "/data"}}{{.Name}}{{end}}{{end}}')"
docker run --rm \
  --mount "source=${data_volume},target=/data,readonly" \
  --mount "type=bind,source=${PWD}/backups,target=/backup" \
  debian:12-slim sh -eu -c \
  'cp /data/custom.db /backup/custom.db; if [ -f /data/custom.db-wal ]; then cp /data/custom.db-wal /backup/custom.db-wal; fi'
docker compose start
```

Before restoring, stop the service and preserve a separate copy of the current
volume. Restore the database and matching WAL together, then start the service
and run `docker exec <container> /app/relational_db --check` with the same
`DB_PATH`/`WAL_PATH` configuration. Do not copy only the database while a
service is running, and do not treat a file copy as an online snapshot.

## Disk full or quota reached

Stop new writes and inspect the service logs and `/api/metrics`. Check the
volume's free space and `MAX_DB_BYTES`/`WAL_MAX_BYTES`; do not delete or
truncate a WAL manually. Free space or expand the volume, then restart if the
service is not ready. Preserve the DB and WAL together for diagnosis. A
configured storage quota may reject a write while reads remain available;
host-level ENOSPC handling and recovery status should be verified against the
current [WAL failure policy](WAL_AND_RECOVERY.md).

## Corrupt page or failed integrity check

Keep the original DB/WAL untouched. Capture logs, revision, and a copy of the
stopped volume. Run `--check` against a copy first. The inspector's
`/api/v1/inspect/pages/{id}` endpoint can report page type/checksum/decode
details; it is diagnostic and does not repair files. Recover from a known-good
backup only after preserving the corrupt files for analysis.

## Upgrade

Review release notes and storage-format compatibility before changing image
tags. Take a stopped-volume backup, deploy the new immutable image digest,
then verify `/readyz`, a representative read, and `--check` on a maintenance
copy. Retain the previous image digest and backup until validation is complete.
Do not run two server instances against the same volume.

## Service objectives and monitoring

This project publishes no production SLA. Suggested operator-owned objectives
for a deployment are:

- Measure `/readyz` externally and choose an availability target appropriate
  for the deployment; an example starting target is 99% over a rolling 30-day
  window. This is not a project guarantee.
- Alert before the data volume or configured DB/WAL quota is exhausted; an
  operator may start with an 80% utilization warning and tune it to the
  storage growth rate.
- Set query-latency objectives from a representative workload. The CI snapshot
  benchmark has a measured p99 threshold below 2 ms for its specified warm
  Release-runner workload; it is not a general-purpose request-latency SLO.
- The WAL crash tests validate recovery of acknowledged commits, but the
  project publishes no recovery-time objective. Measure restart/recovery time
  using the deployment's own database size and storage.

Monitor readiness, HTTP error/rate-limit metrics, WAL/recovery logs, available
volume space, and container restarts. Document the chosen thresholds alongside
the deployment's external monitoring configuration.
