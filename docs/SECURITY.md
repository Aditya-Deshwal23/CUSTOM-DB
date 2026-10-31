# Security policy

CUSTOM-DB is an educational storage engine and is not a hardened
multi-tenant database. Do not expose a writable instance to an untrusted
network without an independent security review and appropriate network
controls.

## Reporting a vulnerability

Please report suspected vulnerabilities privately through
[GitHub Security Advisories](https://github.com/Aditya-Deshwal23/CUSTOM-DB/security/advisories/new)
instead of opening a public issue. Include affected revision, impact, and
reproduction details. Do not include real credentials or private database
contents.

## Deployment safeguards

- Set `API_KEY` for write and administrative routes when access is not
  otherwise restricted; use a secret manager and never paste keys into logs or
  issue reports.
- Configure `CORS_ORIGINS` narrowly. CORS is not authentication.
- Keep request, SQL, result-row, database-size, rate, and statement limits
  enabled at appropriate values; see [CONFIG.md](CONFIG.md).
- Run the container as its configured non-root UID with a read-only root
  filesystem and a dedicated data volume, as in [docker-compose.yml](../docker-compose.yml).
- Treat Render Free's filesystem as ephemeral; do not store important data
  there.

CI builds and scans the image and exercises malformed API inputs, but these
checks do not establish that the engine is free of vulnerabilities.
