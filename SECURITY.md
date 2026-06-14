# Security Policy

The OpenSWMM Engine maintainers take security seriously. This document
describes how to report a vulnerability and what to expect once a report
has been received.

## Supported Versions

Security fixes target the latest **6.x** release line and the active
`develop` branch. The legacy SWMM 5.x C solver carried under
`src/legacy/` is preserved byte-for-byte as a regression baseline; bugs
discovered in that tree are tracked but fixed upstream at EPA whenever
possible.

| Version line     | Supported          |
| ---------------- | ------------------ |
| 6.x (current)    | :white_check_mark: |
| 6.x pre-release  | :white_check_mark: |
| Legacy SWMM 5.x  | :x: (upstream EPA) |

## Reporting a Vulnerability

**Please do not file public GitHub issues for security problems.**

Use one of the private channels below so a fix can be prepared before
public disclosure:

1. **Preferred — GitHub private security advisory.**
   Open a draft advisory in the
   [Security tab](https://github.com/HydroCouple/openswmm.engine/security/advisories/new)
   of this repository. The maintainers receive a notification immediately
   and the advisory remains private until coordinated disclosure.

2. **Email.** If you do not have a GitHub account or cannot use the
   advisory flow, email **security@hydrocouple.org** with the details
   below. Encrypt with the maintainer's PGP key when handling
   exploit-grade payloads.

Please include, where possible:

- A clear description of the issue and the affected component
  (e.g. `src/engine/...`, Python binding, CLI, plugin SDK).
- Affected version, commit SHA, or release tag.
- Reproduction steps, minimal input model (`.inp`) where applicable,
  and the observed vs. expected behaviour.
- Any proof-of-concept exploit, payload, or crash artifact.
- Your assessment of impact (information disclosure, integrity,
  availability, RCE) and which deployment scenarios are exposed.

## What to Expect

- **Acknowledgement** within **3 business days** of report.
- **Initial assessment** (confirmation, severity, scope) within
  **10 business days**.
- **Fix or mitigation plan** communicated within **30 days** of
  confirmation for high/critical issues; lower severities are scheduled
  into the next maintenance release.
- **Coordinated disclosure**: a public advisory and patched release are
  published together. Reporters are credited unless they request
  otherwise.

If a report does not receive a response within the acknowledgement
window, please re-send and CC a maintainer listed in
[`AUTHORS.md`](AUTHORS.md).

## Scope

In scope:

- The refactored C++20 engine (`src/engine/`, `include/openswmm/engine/`)
- Plugin SDK and host-loaded plugin code paths
- Python bindings under `python/openswmm/`
- CLI executables (`src/cli/`, `src/legacy/cli/`)
- Build, packaging, and release tooling under `.github/`,
  `CMakePresets.json`, `vcpkg.json`

Out of scope (will be triaged but not necessarily fixed in this repo):

- Vulnerabilities in `src/legacy/` that exist in upstream EPA SWMM 5.x
  with no OpenSWMM-specific exposure — coordinated upstream.
- Issues in third-party dependencies pulled via vcpkg — please report
  to the dependency's own security channel; we will pin to a fixed
  version once available.
- Denial-of-service via deliberately malformed input models where the
  engine fails safely (no memory corruption, no code execution).

## Hardening Already in Place

- Automated **CodeQL** analysis on every push and PR
  (see [`.github/workflows/codeql.yml`](.github/workflows/codeql.yml)).
- **OpenSSF Scorecard** weekly supply-chain scan
  (see [`.github/workflows/scorecard.yml`](.github/workflows/scorecard.yml)).
- Reproducible builds via pinned `vcpkg` baseline in
  [`vcpkg.json`](vcpkg.json).
- All releases built from tagged commits in CI; no manual artifact
  uploads.
