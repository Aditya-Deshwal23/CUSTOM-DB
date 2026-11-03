# Threat model

## Scope and assets

CUSTOM-DB runs as one Crow service with a page-oriented engine and one
container. The protected assets are database and WAL integrity, API keys,
availability of the service, correctness of query results, release artifacts,
and the provenance of source and container images.

Trust boundaries are: untrusted HTTP clients to Crow; parsed SQL to the
executor; the process to database/WAL files and mounted volumes; the container
to its host; and GitHub-hosted changes/actions to the release registry. The
service is not a tenant boundary and the public demo is not a data-protection
boundary.

## STRIDE by component

| Component | Spoofing | Tampering | Repudiation | Information disclosure | Denial of service | Elevation of privilege |
|---|---|---|---|---|---|---|
| HTTP surface | API-key checks on protected routes; no claim of user identity | JSON/schema validation, bounded bodies and SQL, parameter binding | Request IDs and structured logs | API key redaction; same-origin security headers; no raw inspector bytes by default | Rate limits, statement/result limits, bounded connections and timeouts | READ_ONLY and admin authorization; non-root service |
| Parser | Not an identity provider; caller remains untrusted | Lexer/parser validate grammar and parameter positions | Parse errors carry source position | Errors avoid returning stored tuple data | Configured SQL/depth/token/node/list bounds; grammar fuzzer | AST values are bound/validated before execution |
| Executor | Statement context is per request | Constraints, transaction rollback, typed values and planner checks | Commit metadata and WAL records identify durable changes | Result shape and row bounds; inspector disclosure is separately gated | Statement time, row, memory, result and admission bounds | One exclusive writer gate; no general-purpose extension execution |
| Storage files | Files and DB_PATH/WAL_PATH values are deployment-controlled, not request-controlled | Explicit-endian formats, CRC32C, WAL recovery and integrity check | WAL LSN/commit records | File permissions and private volume are operator responsibilities | Database quota, bounded page/buffer configuration | Container UID 10001; no untrusted file import |
| Container/runtime | Image identity and registry permissions | Digest-pinned base images, image scan, cosign verification | Release tags and signed digest attestations | Read-only root, dropped capabilities, isolated data volume | PID, memory, CPU, tmpfs and file descriptor limits | Non-root UID, no-new-privileges, no writable root filesystem |
| CI/CD | GitHub identities and least-privilege job tokens | SHA-pinned actions, dependency updates, protected release process | Workflow logs, immutable run records, signed image digest | Secret scanning and no secrets in PR code | Job timeouts, concurrency cancellation, bounded scan/fuzz jobs | Scoped workflow permissions, CodeQL, Scorecard and branch protection checklist |

STRIDE concerns are threat categories, not a claim that every attack is
prevented. Network exposure, operator credentials, the host kernel, and
compromise of a trusted GitHub account remain outside the engine's protection.
`DB_PATH` and `WAL_PATH` are trusted deployment configuration; operators must
not populate them from client-controlled values. CodeQL path-injection
findings on these storage opens are accepted on that trust-boundary basis.

## Mitigations and evidence

| Control | Evidence |
|---|---|
| HTTP validation, auth, headers and endpoint contracts | [API tests](../tests/api/test_api.py), [inspector tests](../tests/api/test_inspector.py), [OpenAPI](openapi.yaml), and the `dynamic-security` job in [CI](../.github/workflows/ci.yml) |
| Parser bounds and malformed input handling | [parser tests](../tests/sql_parser_test.cpp), [SQL grammar fuzzer](../tools/sql_grammar_fuzz.cpp), and nightly fuzz job in [nightly.yml](../.github/workflows/nightly.yml) |
| Transaction recovery and storage integrity | [WAL regressions](../tests/regression/known_bugs_test.cpp), [disk manager tests](../tests/disk_manager_test.cpp), and `crash-safety` CI/nightly jobs |
| Release binary mitigations | [hardening verification script](../scripts/check_binary_hardening.sh) and the `hardening` CI job |
| Container image and configuration scanning | `docker-build`, `container-config`, and `dynamic-security` jobs in [CI](../.github/workflows/ci.yml); [Compose configuration](../docker-compose.yml) |
| Source, action, and dependency checks | `gitleaks`, `static-analysis`, CodeQL, Scorecard and Dependabot configuration under [`.github`](../.github) |
| Human-controlled repository protections | [release and settings checklist](RELEASING.md); settings are not asserted until an administrator enables them |

The project has no independent penetration-test certification or production
security warranty. Report suspected vulnerabilities privately through the
root [SECURITY.md](../SECURITY.md).
