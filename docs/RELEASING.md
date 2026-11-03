# Release and repository security

## Release process

1. Merge changes only after required CI checks pass. Confirm that
   `docs/CLAIMS.md`, the release notes, and compatibility notes are current.
2. Create and push an annotated semantic-version tag:

   ```sh
   git tag -a vX.Y.Z -m "CUSTOM-DB vX.Y.Z"
   git push origin vX.Y.Z
   ```

3. The tag workflow scans the image, builds and pushes amd64/arm64 manifests,
   attaches BuildKit SBOM/provenance attestations, signs the image digest with
   GitHub OIDC, verifies that signature, and creates the GitHub release.
4. Confirm the SPDX SBOM, OCI index/attestation manifest, supply-chain summary,
   and recovery recording are attached to the GitHub release.
5. Verify the published digest before deployment:

   ```sh
   cosign verify \
     --certificate-identity-regexp '^https://github\.com/Aditya-Deshwal23/CUSTOM-DB/.github/workflows/release\.yml@refs/tags/v.*$' \
     --certificate-oidc-issuer https://token.actions.githubusercontent.com \
     ghcr.io/aditya-deshwal23/custom-db:<version>
   ```

   The workflow also verifies the immutable digest directly. For local builds,
   use the repository's Docker quickstart and do not claim that a local image
   is a signed release.

### GHCR package visibility (manual)

New GitHub Container Registry packages are private by default. After the first
successful publish, an administrator must open the package page under the
repository's **Packages**, open **Package settings**, and change visibility
to **Public** if the README's unauthenticated pull command is intended to
work. Confirm the change using a clean, unauthenticated Docker client. Do not
store package credentials in the repository.

## Manual GitHub settings checklist

These steps require a repository administrator and are intentionally not
performed by CI:

- [ ] Protect `main`: require a pull request and linear history; disallow force
  pushes; require the four build matrix checks (`gcc / Debug`, `gcc / Release`,
  `clang / Debug`, `clang / Release`), `Address and undefined behavior
  sanitizers`, `Thread sanitizer stress`, `WAL hard-kill recovery (200
  iterations)`, `Formatting and Dockerfile lint`, `Docker image build`,
  `codeql`, and `codeql / python-javascript`.
- [ ] Enable Dependabot alerts and Dependabot security updates.
- [ ] Enable secret scanning and push protection.
- [ ] Enable private vulnerability reporting.
- [ ] Set About description, set Website to the live demo
  (`https://custom-db-engine.onrender.com`), and add topics:
  `database`, `storage-engine`, `b-plus-tree`, `write-ahead-log`, `sql`,
  `cpp17`, `crow`, `docker`, `render`.
- [ ] Upload a project-appropriate social preview image.
- [ ] Review the package visibility step above after the first publish.

Scorecard results depend on repository settings, release history, and GitHub
metadata. The workflow publishes a SARIF report; the requested score threshold
is not considered met until the published Scorecard report shows at least 7.
