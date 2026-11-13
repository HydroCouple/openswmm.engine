# Contributing to openswmm.engine

Thank you for your interest in contributing to **openswmm.engine**! Whether you are reporting a bug, improving the documentation, adding a test case, or proposing a new numerical method, your work helps keep this engine open, reliable, and useful to everyone who depends on it.

This guide explains how the project is governed, how contributions are reviewed and merged, and the standards that keep the engine reproducible and scientifically rigorous. Please read it before opening an issue, submitting a pull request, or proposing a major change.

> **Where contributing can lead.** Contributors are recognised on the [HydroCouple Contributor Ladder](./LADDER.md): Inlet, Lateral, Trunk, Interceptor, and Pump Station. Your first merged change of any kind, even a typo fix, makes you a Lateral. Sustained work, especially verification work, can lead to becoming a Maintainer with merge authority in the parts of the engine you care for. Along the way you can earn [badges](./BADGES.md) that celebrate particular contributions.

---

## Table of Contents

1. [Governance](#1-governance)
2. [Roles, Maintainers & the Contributor Ladder](#2-roles-maintainers--the-contributor-ladder)
3. [Continuity & Urgent Fixes](#3-continuity--urgent-fixes)
4. [Licensing, CLA & Employer Clearance](#4-licensing-cla--employer-clearance)
5. [Recognition & Credit](#5-recognition--credit)
6. [How to Cite openswmm.engine](#6-how-to-cite-openswmmengine)
7. [Versioning Strategy](#7-versioning-strategy)
8. [Deprecation & Backward Compatibility Policy](#8-deprecation--backward-compatibility-policy)
9. [Branching Model](#9-branching-model)
10. [Bug Fixes & Small Improvements](#10-bug-fixes--small-improvements)
11. [Pull Request Process](#11-pull-request-process)
12. [Continuous Integration & Automated Testing](#12-continuous-integration--automated-testing)
13. [Issue Reporting](#13-issue-reporting)
14. [Review Timeline Expectations](#14-review-timeline-expectations)
15. [Major Project Formulation Changes](#15-major-project-formulation-changes)
16. [Conflict of Interest in Reviews](#16-conflict-of-interest-in-reviews)
17. [Verification & Benchmark Testing](#17-verification--benchmark-testing)
18. [Documentation Standards](#18-documentation-standards)
19. [Dependency Evaluation Policy](#19-dependency-evaluation-policy)
20. [Security Vulnerability Reporting](#20-security-vulnerability-reporting)
21. [Roadmap & Prioritization](#21-roadmap--prioritization)
22. [Code of Conduct](#22-code-of-conduct)

---

## 1. Governance

openswmm.engine is an open, community-driven project. Everyone is welcome to take part, regardless of affiliation, background, or experience level. Discussion happens in the open on GitHub, and decisions are explained publicly.

The engine is stewarded by **HydroCouple Foundation, Inc.**, a nonprofit in formation whose purpose is to develop and maintain open infrastructure for environmental modeling. The Foundation's Bylaws set out who makes which decisions, so that every decision has one clear owner:

- **The Executive Director, Caleb Buahin ([@cbuahin](https://github.com/cbuahin)),** holds final authority over the engine's technical direction: architecture, standards, the roadmap, repository administration, merges, and releases. The Executive Director exercises this authority directly and through the Maintainers.
- **The Chief Scientist, Corinne Wiesner-Friedman ([@corinne-wf-home](https://github.com/corinne-wf-home)),** leads verification, validation, and benchmark standards for the whole engine and certifies each release. The Chief Scientist also holds final authority within a set of **Designated Subsystems** (see below).
- **Maintainers** review, merge, and release changes within the scopes they are assigned (Section 2).
- **The Technical Steering Committee** advises on priorities and reviews draft standards. Its advice is recorded and answered publicly, and it does not direct the work. One of its seats is elected by the contributor community (Section 2).
- **The Board of Directors** oversees the Foundation's finances, legal compliance, and mission. It does not direct technical work.

### Designated Subsystems

Within these subsystems, the Chief Scientist has final authority over design, merges, releases, the Maintainers who work on them, and the verification standards that apply to them:

| Designated Subsystem | Where it lives in this repository |
|---|---|
| Uncertainty quantification, including the uncertainty sidecar and operator snapshot layer | On the `feature/uncertainty-sidecar` branch; paths are added here when it merges |
| Contaminant, sediment, and reactive fate and transport | `src/engine/quality/` |
| Verification, validation, and benchmark suites | `tests/benchmarks/`, `tests/regression/`, `tests/regression_testing/` |
| Test infrastructure and the continuous verification pipeline | `tests/CMakeLists.txt`, `tests/helper.h`, and the testing workflows in `.github/workflows/` |

Everything else is within the Executive Director's authority. Within a Designated Subsystem, the Executive Director's role covers integration with the rest of the engine: the build system, public interfaces, interfaces between subsystems, dependency policy, and release scheduling.

[`CODEOWNERS`](./.github/CODEOWNERS) applies this map to every pull request, so the right people are asked to review automatically. If it is unclear whether a change falls within a Designated Subsystem, the Chief Scientist decides, as the Bylaws provide.

> **While the Foundation is being formed.** Until HydroCouple Foundation is incorporated and its Bylaws are adopted, Caleb Buahin exercises the authority described here for the Executive Director, as the project's Technical Manager, and Corinne Wiesner-Friedman exercises the authority described for the Chief Scientist.

---

## 2. Roles, Maintainers & the Contributor Ladder

### The Contributor Ladder

Contributors are recognised on a public ladder, described in full in [LADDER.md](./LADDER.md). Ranks reflect sustained and verifiable work, such as tests, reviews, and benchmark cases with documented results, rather than the number of pull requests or lines of code.

| Rank | What it means | GitHub permission |
|---|---|---|
| **Inlet** | You have taken part: opened an issue, joined a discussion, or asked a question | Read |
| **Lateral** | You have had a contribution of any kind merged | Read |
| **Trunk** | You have contributed across two or more release cycles | Triage |
| **Interceptor** | You care for part of the verification corpus and review others' work | Triage, plus automatic review requests in your area |
| **Pump Station** | You are a **Maintainer**, with merge authority in your assigned scope | Write |

Contributors at Trunk rank and above are **Recognized Contributors**. Every two years they elect one member of the Foundation's Technical Steering Committee, and any Recognized Contributor may stand for that seat.

### Maintainers

Maintainers write, review, merge, and release changes within their scope. A Maintainer is designated in writing by the **appointing officer** for the scope: the Chief Scientist for a Designated Subsystem, and the Executive Director for everything else. The criteria in [LADDER.md](./LADDER.md) describe what the appointing officers look for.

Before designation, a Maintainer has on file a signed CLA, any employer clearance their work requires (Section 4), a Conflict of Interest disclosure, confirmation that they use a personal GitHub account with two-factor authentication, and acknowledgement of the Code of Conduct and the Maintainers Charter.

| Maintainer | Scope |
|---|---|
| Caleb Buahin ([@cbuahin](https://github.com/cbuahin)) | All scopes outside the Designated Subsystems (by office as Executive Director) |
| Corinne Wiesner-Friedman ([@corinne-wf-home](https://github.com/corinne-wf-home)) | The Designated Subsystems (by office as Chief Scientist) |

**Infrastructure and credentials**, which covers organization settings, branch protection, signing keys, package credentials, and CI secrets, is a Maintainer scope held by the Executive Director and one or two other Maintainers named in the Foundation's continuity plan. It is assigned by designation rather than reached through the ladder.

Scopes change only by a further written designation from the appointing officer. When a Maintainer steps back, their scope passes to the successor named for it, and they are listed as an emeritus Maintainer. Their rank and their credit are unchanged.

---

## 3. Continuity & Urgent Fixes

The project is set up so that its work continues if any one person becomes unavailable.

- **Successors.** Each officer and each Maintainer scope has a designated successor in the Foundation's continuity plan. If someone is unavailable, for any reason, their successor acts for them until they return.
- **If an officer is unavailable** and no successor has been designated, the Chief Scientist's duties pass to the Executive Director, and the Executive Director's technical duties pass to the Chief Scientist.
- **Urgent security fixes.** A release that fixes a critical or actively exploited security vulnerability may be issued on the Executive Director's authority without waiting for verification certification. Certification status is published within fourteen days. If the fix touches a Designated Subsystem, the Chief Scientist is told before the release where practicable, and in any event within 24 hours.
- **Release signing and administration** require two people's credentials. Within twelve months of the Foundation's first public release, at least one of them will be someone other than the two founding officers.

---

## 4. Licensing, CLA & Employer Clearance

openswmm.engine is released under the license in [LICENSE](./LICENSE) (currently the **MIT License**). Portions of the engine derive from the Storm Water Management Model developed by the U.S. Environmental Protection Agency, which is in the public domain. The project's license applies only to original contributions.

### Contributor License Agreement

Every contributor signs the **Contributor License Agreement (CLA)** in [CLA.md](./CLA.md) before their first pull request is merged. This applies to everyone, including the project's officers and Maintainers. Key points:

- **You keep your copyright.** The CLA grants a license; it does not transfer ownership.
- **The license is non-exclusive.** The project can never hold exclusive rights in your contribution, and contributions are distributed only under open-source licenses approved by the Open Source Initiative.
- **Patent grant.** You grant a royalty-free patent license covering patents necessarily infringed by your contribution.
- **Authority.** You confirm you have the right to submit the contribution, including any permission your employer requires.

When the Foundation is formed, the rights granted under the existing CLA pass to the Foundation, and the CLA is updated so that future grants run to the Foundation directly. Any change to the CLA will be announced in advance.

**How to sign.** You can sign in any of these ways, and you only need to sign once:

1. Follow the link that the CLA Assistant bot posts on your first pull request.
2. Post this comment on your pull request from your own account: `I have read the CLA Document and I hereby sign the CLA.`
3. Email a Maintainer from your personal address with your name, GitHub username, the CLA version, and the sentence above.

Signatures are recorded in `CLA-SIGNATORIES.md`.

### Contributions connected to your employer

If you are employed, and your contribution relates to your employer's business or was made using your employer's time or equipment, we need your employer's clearance before it can be merged. That means either a **Corporate CLA** ([CCLA.md](./CCLA.md)) signed by your employer, or a written authorization from someone at your employer with authority to give it.

The pull request template asks about your employment status so that this can be sorted out early. If clearance is needed, review continues as normal and the pull request is labelled `quarantine:employer-clearance` until the clearance arrives. We are glad to help: we can provide the Corporate CLA and a model authorization letter for your employer.

### Personal accounts

Please contribute from a GitHub account and commit email address that are personal to you, not ones administered by an employer or tied to an employer's single sign-on. If you have contributed from more than one account, let us know so your contributions can be credited together.

If your contribution includes third-party code or data, make sure its license is approved by the Open Source Initiative and compatible with the project's license, and list it in `NOTICE`.

---

## 5. Recognition & Credit

Your contributions are credited in several places, each with its own purpose:

| Record | What it shows |
|---|---|
| [**AUTHORS.md**](./AUTHORS.md) | Everyone whose contribution has been merged, generated from the commit history at each release. Nobody is ever removed from it. |
| **Contributors table** ([CONTRIBUTORS.md](https://github.com/HydroCouple/community-recognition/blob/main/CONTRIBUTORS.md)) | Everyone who has earned a badge, with links to the work behind each one |
| [**ROSTER.md**](./ROSTER.md) | Contributor ranks, the history of promotions, and the HydroCouple Awards |
| [**CITATION.cff**](./CITATION.cff) | How to cite the engine; Interceptors and above are included as authors of each release's DOI once per-release DOIs are available |
| **Release notes** | Contributors to each release |

You don't need to add yourself to any of these. They are updated for you.

### Badges

Badges celebrate particular contributions: a first merged pull request, a verification case with documented results, a clear bug report, a translation, a discussion that became a roadmap item, and many more. **You never need to ask for a badge.** Most are found automatically from GitHub activity, and a project Maintainer reviews them each week before they are awarded. The full list, and how each is earned, is in [BADGES.md](./BADGES.md). Badges are a celebration only: they carry no permissions and do not count toward ranks.

### The Annual Community Meeting and HydroCouple Awards

Once a year, the Foundation holds an open **Annual Community Meeting**, where the officers report on the roadmap, releases, verification, and finances and answer questions from the community. The meeting is also where the **HydroCouple Awards** are presented, such as the HydroCouple Award for Education and Outreach. An independent panel of respected people in the field chooses the recipients. See [LADDER.md](./LADDER.md#the-annual-community-meeting).

If you think a contribution has been missed or described incorrectly in any of these records, please open an issue.

---

## 6. How to Cite openswmm.engine

If you use openswmm.engine in research, teaching materials, or engineering reports, please cite it. Citations credit everyone who built the engine and help others find it.

- **Use the citation in [CITATION.cff](./CITATION.cff).** On GitHub, the **"Cite this repository"** button in the sidebar provides it in APA and BibTeX formats.
- **Release DOIs.** Per-release DOIs through [Zenodo](https://zenodo.org) are being set up. Once they are available, each stable release's DOI will be listed on the [Releases](https://github.com/HydroCouple/openswmm.engine/releases) page. When citing a specific version, please use its DOI so your work is reproducible.
- **Publications.** If a peer-reviewed publication describes a formulation you used, please cite it as well. If you publish work describing a contribution to the engine, let us know so we can add the reference.

If you build a project, product, course, or service on openswmm.engine, please keep the license notice wherever you distribute the engine, name and link openswmm.engine where your users will see it, and cite it as above. Projects that do all three are eligible for the [Upstream Credit](./BADGES.md#upstream-credit) badge.

---

## 7. Versioning Strategy

openswmm.engine follows **Semantic Versioning (SemVer)** as defined at [semver.org](https://semver.org), with formal pre-release stages during testing.

### Version Format

```
MAJOR.MINOR.PATCH[-PRERELEASE]
```

| Component | When to increment |
|-----------|-------------------|
| `MAJOR`   | Incompatible API or formulation changes; fundamental engine restructuring |
| `MINOR`   | New features or model components added in a backward-compatible manner |
| `PATCH`   | Backward-compatible bug fixes, documentation updates, or performance improvements |

### Pre-Release Stages

During release testing, the following pre-release labels are used in sequence:

| Stage              | Label example     | Purpose                                                                      |
|--------------------|-------------------|------------------------------------------------------------------------------|
| Alpha              | `1.2.0-alpha.1`   | Early feature-complete builds, internal or limited testing; may be unstable  |
| Beta               | `1.2.0-beta.1`    | Feature-frozen; broader community testing; known issues may exist            |
| Release Candidate  | `1.2.0-rc.1`      | Final candidate for release; only critical bug fixes accepted                |
| Stable             | `1.2.0`           | Fully validated, production-ready release                                    |

Pre-release builds are tagged in Git and published with clear labels so they are not mistaken for production releases.

### Release certification

Before each release, the Chief Scientist certifies in writing whether it meets the published verification standard, and states the reasons if not:

- **Major releases** require certification. If certification is declined, the Foundation's Board may still authorize the release by a supermajority vote, after hearing the Chief Scientist's reasons and the Technical Steering Committee's advice. The release notes then reproduce the reasons in full.
- **Other releases** may proceed without certification on the Executive Director's authority. The Board is notified beforehand, and the release notes state that certification was withheld and give the reasons in full.

Either way, any release made without certification says so openly.

---

## 8. Deprecation & Backward Compatibility Policy

openswmm.engine is committed to giving users adequate notice before breaking changes are introduced.

### Deprecation Process

1. **Announcement.** A feature, API, or behavior to be removed is first marked as deprecated in the release notes and in code (through comments or compiler warnings, as applicable). The announcement states the planned removal version.
2. **Minimum notice period.** Deprecated items are not removed for at least **one full minor version cycle** after the announcement. For widely used public APIs, a minimum of **one major version cycle** is preferred.
3. **Removal.** Deprecated items are removed in a `MAJOR` version, never in a `MINOR` or `PATCH` release.

### Backward Compatibility Commitment

- `PATCH` releases are fully backward-compatible.
- `MINOR` releases are backward-compatible for all stable public APIs.
- `MAJOR` releases may introduce breaking changes, documented in a migration guide published with the release.

Experimental APIs (clearly marked as such) carry no backward compatibility guarantee and may change or be removed in any release.

---

## 9. Branching Model

| Branch                    | Purpose                                                                                     |
|---------------------------|---------------------------------------------------------------------------------------------|
| `main`                    | Stable, production-ready code. Receives merges from release branches.                      |
| `develop`                 | Integration branch for ongoing development. Feature and bug-fix branches target here.      |
| `release/<x.y>`           | Release preparation (alpha → beta → rc), cut from `develop`.                               |
| `swmm6_rel`               | The current 6.x release-preparation branch, until 6.x moves to `release/<x.y>` branches.   |
| `swmm6_exp`               | The current 6.x experimental branch.                                                        |
| `experimental/<name>`     | Sandboxed branches for exploratory or major formulation changes. See Section 15.           |
| `bugfix/<issue-id>-desc`  | Short-lived branches forked from `develop` to address specific bug reports.                |
| `feature/<name>`          | Short-lived branches for minor feature additions, forked from `develop`.                   |

The default and release branches are protected: changes arrive only by pull request, the CLA check and required tests must pass, and force pushes are blocked.

---

## 10. Bug Fixes & Small Improvements

For small, well-scoped bug fixes and minor improvements:

1. **Open or reference an issue.** Check that the bug is reproducible, and link the issue in your commits and pull request.
2. **Branch from `develop`.** Name your branch `bugfix/<issue-id>-short-description` (for example `bugfix/42-overflow-in-routing`).
3. **Write a failing unit test first.** The test should demonstrate the defect before the fix is applied.
4. **Make the minimal fix.** Change only what is needed to resolve the issue, and leave refactoring of nearby code for a separate pull request.
5. **Check the benchmarks.** All analytical benchmarks must pass. If your fix changes results on any reference problem, either correct the unintended change, or include a benchmark update in the pull request: documentation of the improvement and a test demonstrating it (Section 17).
6. **Open a pull request** against `develop` following Section 11.

---

## 11. Pull Request Process

All contributions enter the codebase through a pull request (PR).

### Review and merge

- **Approval.** A PR merges into the default branch after approval by at least one Maintainer whose scope covers the change and who is not an author of it, and after all required checks pass, including the CLA check.
- **Release branches.** A change merges into a release branch, or is included in a tagged release, after approval by two Maintainers within scope, neither of whom is an author of the change. Where fewer than three Maintainers hold the scope, the appointing officer may authorize the merge with one approval and records this in the release record.
- **No self-merging.** Maintainers do not merge their own substantive changes. A substantive change is any change to source code, numerical methods, interfaces or data models, build configuration, dependencies, CI configuration, or licensing or notice files. Documentation-only changes that don't alter normative content may be merged by their author once the automated checks pass.
- **Final decisions.** Outside the Designated Subsystems, the Executive Director may override a review outcome; within them, the Chief Scientist may. Any override is recorded on the pull request.
- **Community reviews are warmly encouraged.** Anyone may review a pull request, and thoughtful reviews are one of the most valuable ways to contribute. They also count toward the [Contributor Ladder](./LADDER.md).
- **AI review.** An automated AI-assisted review (GitHub Copilot) runs on every PR. Please address or respond to its comments. It does not count as an approval.

[`CODEOWNERS`](./.github/CODEOWNERS) requests reviews from the Maintainers for the paths a PR touches, including the Designated Subsystems (Section 1).

### PR Checklist

Before requesting review, please confirm that your PR:

- [ ] Targets the correct branch (`develop` for bug fixes and features; see Section 15 for experimental work)
- [ ] Is covered by your signed CLA, and by employer clearance if your contribution is connected to your employer (Section 4)
- [ ] Answers the employment-status question in the PR template
- [ ] Includes a clear description of what changed and why
- [ ] References any related issues (for example `Closes #42`)
- [ ] Includes new or updated unit tests covering the change
- [ ] Passes all analytical benchmarks, and leaves benchmark results unchanged or includes a documented benchmark update (Section 17)
- [ ] Passes the CI pipeline (Section 12)
- [ ] Addresses or responds to the AI review's comments
- [ ] Updates documentation where applicable (Doxygen comments, README, etc.)
- [ ] Lists any third-party code or data in `NOTICE`

The PR template also has an optional **Referred by** field. If someone encouraged you to contribute, you can name them there.

### Merge style

- Squash merging is preferred for `bugfix` and `feature` branches to keep the `develop` history clean.
- Merge commits are used when integrating `develop` into a release branch to preserve the full history.

---

## 12. Continuous Integration & Automated Testing

All pull requests are tested automatically by the project's CI pipeline (GitHub Actions) before human review begins. A failing pipeline blocks merging.

The pipeline runs on every push to a PR branch and includes:

| Stage                      | Description                                                                                   |
|----------------------------|-----------------------------------------------------------------------------------------------|
| **CLA check**              | Confirms every commit author has signed the CLA                                               |
| **Build**                  | Compiles the engine and test suite on all supported platforms (Linux, macOS, Windows)        |
| **Unit Tests**             | Runs the full unit test suite; any failure blocks the PR                                      |
| **Analytical Benchmarks**  | Compares results with exact solutions; deviations outside tolerance block the PR             |
| **Benchmark Comparison**   | Compares results on reference problems with the current benchmark; any change outside tolerance blocks the PR unless it includes a benchmark update (Section 17) |
| **Static Analysis**        | Runs a linter and static analyzer (for example `clang-tidy`) to catch common errors          |
| **Documentation Build**    | Checks that the Doxygen documentation builds without errors or warnings                       |

Please run the test suite locally before opening a PR. Instructions are in the [README](./README.md).

---

## 13. Issue Reporting

### Bug Reports

Please use the **Bug Report** issue template. A complete bug report includes:

- A clear, descriptive title.
- The version of openswmm.engine affected (for example `1.2.0-beta.1`).
- Your operating system and compiler version.
- A minimal reproducible example: the smallest input file or code snippet that shows the problem.
- What you observed and what you expected, including any output or error messages.

Reports without enough detail to reproduce the problem may be paused until more information is available.

If your example uses a model of a real system, please share only what you're comfortable making public. Issues are public, and synthetic or anonymized examples work well.

### Feature Requests & Ideas

Use the **Feature Request** template for small, well-scoped additions. For proposals that would change the engine's physical or mathematical formulation, please start a thread in the GitHub Discussions **Ideas** section instead (see Section 15).

### Issue Triage

Contributors at Trunk rank and above can triage issues. We aim to label new issues within **two weeks**, assigning a type (for example `bug`, `enhancement`, `needs-clarification`, or `wontfix`) and a priority. Some labels, such as `reproducer-confirmed`, also help recognise contributions with badges (see [BADGES.md](./BADGES.md#for-triagers-and-reviewers)). Please search for existing issues before opening a new one.

---

## 14. Review Timeline Expectations

openswmm.engine is maintained by a small team, and many contributors volunteer their time. These are our targets:

| Action                                    | Target Timeline          |
|-------------------------------------------|--------------------------|
| Initial issue triage                      | Within 2 weeks           |
| First review response on a PR             | Within 3 weeks           |
| Follow-up review after requested changes  | Within 2 weeks           |
| Major formulation discussion response     | Within 4 weeks           |

If you haven't heard back within these windows, a friendly follow-up comment on the issue or PR is welcome. PRs with no activity for 60 days may be closed, with a note that they can be reopened whenever you're ready to continue.

---

## 15. Major Project Formulation Changes

Changes to the engine's physical or mathematical formulation, such as new process models, numerical schemes, or significant algorithmic restructuring, follow a more thorough process to protect the engine's scientific integrity.

### Step 1: Open a Community Discussion

Start a thread in the **Ideas** section of GitHub Discussions. Your proposal should describe:

- The scientific or engineering motivation for the change.
- The expected impact on existing functionality and results.
- Known limitations, tradeoffs, or open questions.
- References to relevant literature or prior implementations.

### Step 2: Community Feedback

Allow at least **four weeks** for the community to respond. The Executive Director, or the Chief Scientist for a Designated Subsystem, facilitates the discussion and summarizes the consensus or the main points of disagreement. A proposal may be:

- **Accepted for prototyping:** there is enough merit to proceed.
- **Revised:** further refinement is needed before prototyping.
- **Deferred:** not a current priority; it may be revisited in a future release cycle.
- **Declined:** outside the project's scope, or not scientifically sound.

A proposal accepted for prototyping earns its author the [Datum](./BADGES.md#datum) badge.

### Step 3: Experimental Branch Implementation

Accepted proposals are developed in a dedicated `experimental/<name>` branch, with:

- Thorough **unit tests** covering the new formulation's behavior across the expected parameter space.
- **Benchmark tests** as described in Section 17.
- Code that compiles cleanly and doesn't break the existing test suite on `develop`.
- Implementation notes and algorithm descriptions in Doxygen-compatible documentation.

### Step 4: Peer Review (Strongly Encouraged)

Where the improvement is a meaningful scientific advance, we strongly encourage a **peer-reviewed publication** describing the formulation, validation methodology, and results. Publication isn't required, but it strengthens the case for adding the work to the core engine and signals that it is ready for production use. The Executive Director and the Chief Scientist are happy to advise on suitable journals or conferences.

### Step 5: Verification & Benchmark Testing

See Section 17 for details. At minimum, the implementation must demonstrate:

- Correctness against problems with known analytical solutions.
- A documented benchmark update for every reference problem whose results change, with tests demonstrating that the new results are an improvement.
- No unacceptable loss of computational performance or numerical stability.

### Step 6: Documentation & References

Before a major formulation change can be merged into `develop`, the following must be complete:

- **Doxygen comments** on all new and modified public APIs, structs, and functions.
- **Inline citations** to peer-reviewed references where the formulation comes from published work.
- Updated user documentation (for example README sections or wiki pages) describing the new capability and how to use it.
- A changelog entry summarizing the change for the next release notes.

---

## 16. Conflict of Interest in Reviews

Scientific software reviews can touch on reviewers' professional interests. To keep reviews fair:

- If you have a competing or closely related implementation, an active publication, or a commercial or financial interest in the outcome of a review, including through your employer, please **disclose it on the pull request** before taking part.
- A reviewer with such an interest may join the discussion but **does not approve** the change.
- Maintainers and officers also file a Conflict of Interest disclosure with the Foundation each year, and do not approve, merge, block, or deprioritize changes in which they or their employer hold a competitive or commercial interest.
- If a disclosed interest raises a question about who should review, the Executive Director decides, or the Chief Scientist within a Designated Subsystem.
- If an undisclosed conflict comes to light after a merge, the contribution may be reviewed again.

---

## 17. Verification & Benchmark Testing

Verification is at the heart of openswmm.engine. The Chief Scientist sets and publishes the engine's verification standard and certifies each release against it (Section 7). Results are tested in two complementary ways.

openswmm.engine does not treat agreement with earlier versions of SWMM as proof of correctness. Earlier versions, including EPA SWMM, contain known defects, and reproducing their output would carry those defects forward. Instead, results are checked against exact solutions where they exist, and otherwise against a benchmark that moves when a change is shown to be an improvement.

### Mode 1: Analytical Benchmarks

These test cases have a closed-form or exact analytical solution. The engine's output must agree with the solution within a tolerance defined for each case. These tests check the mathematical correctness of the implementation independently of any other model. An analytical solution changes only if it is itself shown to be wrong, and that correction is documented in the same way as a benchmark update.

Examples include mass balance checks, steady-state flow solutions, unit hydrograph convolutions, manufactured solutions, and simplified routing scenarios with exact solutions.

### Mode 2: Shifting Benchmarks

For realistic problems that have no exact solution, the project keeps a set of **reference problems** and the engine's **current accepted results** for them: the *benchmark*. The benchmark records the engine's own best current answer, not the output of another model or a frozen earlier release.

CI compares every pull request with the current benchmark, and any change in results outside a problem's tolerance blocks the pull request. That way, no result ever changes by accident.

A result can change, and the benchmark move with it, through a **benchmark update** in the same pull request. A benchmark update includes:

1. **Documentation of the improvement:** what was wrong or limited before, what changed, and why the new results are more correct, with references to the literature, the defect report, or the derivation.
2. **A test demonstrating the improvement:** an analytical or manufactured-solution case (Mode 1), a conservation or stability check, or a comparison with documented field or laboratory data, which the old results fail and the new results pass.
3. **The updated benchmark** for every affected reference problem, with the size of each change summarised in the pull request.
4. **A changelog entry** under *Benchmark changes*, so users can see when and why results changed between versions.

The benchmark suites are a Designated Subsystem, so the Chief Scientist, or a Maintainer in that scope, reviews every benchmark update.

A change that alters results without a demonstrated improvement is a regression, however small. Catching one before it reaches a release earns the [Freeboard](./BADGES.md#freeboard) badge.

### Comparisons with other models

Each release's verification report includes comparisons with EPA SWMM and with earlier releases of openswmm.engine, so that users and reviewing agencies can see how results differ and why. These comparisons are published for information. They are not pass/fail criteria, and they never block a pull request. Where openswmm.engine deliberately differs from EPA SWMM because a defect has been corrected, the benchmark update that introduced the difference explains it.

### Benchmarking Criteria

All benchmarks are evaluated on three dimensions:

| Criterion                      | Description                                                                                      |
|--------------------------------|--------------------------------------------------------------------------------------------------|
| **Stability**                  | The solver must not show divergence, oscillation, or blow-up under standard test conditions      |
| **Conservativeness**           | Mass, volume, or energy conservation must be maintained within defined tolerances                |
| **Computational Performance**  | Runtime must not degrade unacceptably relative to the previous version for equivalent problem sizes |

Performance benchmarks run on a reference hardware configuration documented in the test suite. Significant performance regressions need a justification and, where possible, mitigation.

### Verification ownership

Each subsystem's verification cases have a named owner, at Interceptor rank or above, who looks after the cases, their documented results, and the subsystem's reference problems. Owners are assigned by the Chief Scientist.

---

## 18. Documentation Standards

openswmm.engine uses **Doxygen** for API documentation. All public code elements (functions, classes, structs, enumerations, and their members) need Doxygen-compatible documentation comments.

Minimum documentation per element:

- `@brief`: a one-line summary.
- `@param`: a description of each parameter, including units where applicable.
- `@return`: a description of the return value.
- `@note` / `@warning`: any important behavioral caveats.
- `@ref` or `@cite`: a citation of the relevant literature where the implementation follows a published formulation.

Documentation is reviewed as part of each pull request, and public APIs need documentation before they can be approved.

---

## 19. Dependency Evaluation Policy

Each new third-party dependency adds maintenance work and risk, so proposed dependencies are evaluated against these criteria:

| Criterion                | Requirement                                                                                      |
|--------------------------|--------------------------------------------------------------------------------------------------|
| **License compatibility**| The license must be approved by the Open Source Initiative and compatible with the project's license (for example MIT, BSD, or Apache 2.0). Copyleft licenses such as GPL are not permitted in the core engine. |
| **Maintenance status**   | The dependency must be actively maintained, with a responsive upstream community.               |
| **Stability**            | The dependency must have a stable, versioned API. Unpinned or volatile dependencies are not accepted. |
| **Binary footprint**     | Dependencies that substantially increase compiled binary size need a proportionate benefit.      |
| **Platform support**     | The dependency must support all officially targeted platforms (Linux, macOS, Windows).           |

Please raise a proposed dependency in a GitHub Discussion or issue before implementing it. Optional or experimental dependencies that aren't required for the core build may be included behind a feature flag with lighter review, provided they meet the license requirement. Every dependency is listed in `NOTICE`.

---

## 20. Security Vulnerability Reporting

**Please do not report security vulnerabilities in public GitHub issues.**

If you find a potential security vulnerability in openswmm.engine, please report it privately so a fix can be prepared before it is disclosed:

- **GitHub Private Advisory:** Use the [Security Advisories](https://github.com/HydroCouple/openswmm.engine/security/advisories) tab to submit a draft advisory, and tag the Executive Director, [@cbuahin](https://github.com/cbuahin).

Please include:

- A description of the vulnerability and its potential impact.
- Steps to reproduce, including any relevant input files or code.
- The affected version(s) of openswmm.engine.
- Any known mitigations or workarounds.

You will receive an acknowledgement within **five business days**, and we will work with you on a coordinated disclosure timeline. Responsible disclosures are credited in the release notes and security advisory unless you prefer to remain anonymous. See [SECURITY.md](./SECURITY.md) for more.

---

## 21. Roadmap & Prioritization

The project keeps a public roadmap in [ROADMAP.md](./ROADMAP.md), maintained by the Executive Director, with the Chief Scientist reporting on the Designated Subsystems. The roadmap covers:

- Features and formulation improvements planned for upcoming `MINOR` and `MAJOR` releases.
- Known issues and technical debt scheduled for resolution.
- Research directions being explored on experimental branches.
- Items deferred or declined, with the reasons.

The roadmap is updated at each release and after significant community discussions. Proposals outside the current roadmap are still welcome and are considered on their merits, though they may be scheduled for a later cycle.

There are several ways to help shape it:

- **Everyone** can join the GitHub Discussions **Ideas** section. Threads that shaped a decision are referenced in the roadmap.
- **Anyone** can describe a recurring problem in modeling practice that the engine could help with, using the pain-point template. Each submission is reviewed and its status recorded.
- **The Technical Steering Committee** advises on priorities, and its advice and the response to it are published. Recognized Contributors elect one of its members.

---

## 22. Code of Conduct

openswmm.engine is committed to a welcoming, respectful, and inclusive environment for everyone. All participants are expected to follow the [Code of Conduct](./CODE_OF_CONDUCT.md), and in particular to:

- Engage constructively and professionally in all project spaces (issues, pull requests, discussions).
- Respect differing viewpoints and scientific perspectives.
- Accept constructive criticism of their contributions in good faith.
- Put the long-term health of the project ahead of individual preferences.

Harassment, personal attacks, and exclusionary behavior are not tolerated. Please report concerns to the Executive Director, [@cbuahin](https://github.com/cbuahin). Reports concerning either officer go to the Foundation's Governance and Nominating Committee at [address to be published], which does not pass through either officer. All reports are handled promptly and confidentially, and no one who reports in good faith will face retaliation.

---

*EPA SWMM is a product of the U.S. Environmental Protection Agency. HydroCouple Foundation is not affiliated with or endorsed by EPA.*

*This document is maintained by the Executive Director of openswmm.engine. Last updated: October 2026 (draft).*
