# Security policy

CUSTOM-DB is an educational storage engine, not a hardened multi-tenant
database. Do not expose a writable instance to an untrusted network or use it
for sensitive or production data.

## Supported versions

Security fixes are applied to the latest `main` branch and the latest
published release. Older release tags are unsupported; upgrade to the latest
release after reviewing its notes and storage-format compatibility.

## Private reporting

Use [GitHub private vulnerability reporting](https://github.com/Aditya-Deshwal23/CUSTOM-DB/security/advisories/new).
Do not open a public issue for an unpatched vulnerability. Include the
affected revision, impact, and a minimal sanitized reproduction. Never attach
credentials or private database contents.

## Response targets

Targets are measured from receipt of a complete private report:

| Activity | Target |
|---|---|
| Acknowledge the report | Within 3 business days |
| Initial triage and severity assessment | Within 7 calendar days |
| Critical/high mitigation or release plan | Within 30 calendar days |
| Medium/low remediation plan | Within 90 calendar days |

These are response goals, not a guarantee of a release date. Affected
reporters will be kept informed when a fix or workaround is available.

For deployment safeguards and operational response, see
[docs/OPERATIONS.md](docs/OPERATIONS.md) and
[docs/THREAT_MODEL.md](docs/THREAT_MODEL.md).
