# Privacy Statement — openswmm.engine

**Version 1.0 — last updated October 3, 2026**

This statement explains what personal information the openswmm.engine software handles, and what personal information the project keeps about the people who contribute to it. In short:

- **The software collects no personal information for us.** It has no accounts, telemetry, analytics, crash reporting, advertising, or update checks, and it never contacts a project-run server. It runs on your own machine. The only network connections are ones you set up yourself, described in Section 3.
- **We keep a small amount of information about contributors**, mainly GitHub identities and contributor-agreement records. Most of it is public by nature, because it lives in a public Git repository.
- **We never sell personal information or share it for advertising.**

---

## 1. Who is responsible

This project is maintained under the HydroCouple GitHub organization (<https://github.com/HydroCouple>). It is to be stewarded by the **HydroCouple Foundation**, a nonprofit that **has not yet been formed**. Until the Foundation is incorporated, the person responsible for personal information handled by the project is **Caleb Buahin** (GitHub: [@cbuahin](https://github.com/cbuahin)), its founding Executive Director. Where data protection law uses the term, he is the *controller*. When the Foundation is incorporated, that responsibility will pass to it and we will update this statement.

The project does not process personal information on behalf of anyone else. In particular, it does not receive, host, or run your models or data.

**Contact:** [support@hydrocouple.org](mailto:support@hydrocouple.org) for any privacy question or request.

This statement covers the software in this repository and the project's own handling of contributor information. It does not cover:

- **GitHub's own processing.** GitHub hosts the code and the contribution tools; see the [GitHub General Privacy Statement](https://docs.github.com/site-policy/privacy-policies/github-general-privacy-statement).
- **Third-party services you choose to connect the software to**, which are described in Section 3.
- **Software built by others** on top of this project.

---

## 2. What the software collects

Nothing, for us. Using the software sends no information to the project, and the project receives no information about who uses it or how. Anything the software stores stays on your device, and anything it transmits goes only to services you choose. The details for this package follow.

---

## 3. This package: openswmm.engine

The OpenSWMM Engine is a C++ simulation library, a command-line program, and the `openswmm` Python package. It has no accounts, telemetry, analytics, crash reporting, or update checks, and it never contacts a project-run server.

**Network connections: none.** The engine, the command-line program, and the Python package make no network connections. The HDF5 library is built without its remote (S3/HTTP) drivers. The Python package depends only on NumPy, and the optional GPU companion is found among packages you have installed; nothing is downloaded at run time.

**Files on your device.**

- The engine reads and writes only the files you name: model input, report (`.rpt`), binary output (`.out`), HDF5, GeoPackage, hot-start, and interface files.
- **Output files can contain information from your inputs.** For example, a report lists the path of each rain-gage data file. Check outputs before sharing them.
- Reading a GeoPackage creates a scratch folder next to it (`<name>.scratch/`).
- The bundled legacy SWMM 5 engine creates temporary files in your system's temporary directory.
- Neither engine records your user name, computer name, or location.

**Optional diagnostics and plug-ins, all off unless you turn them on with environment variables.**

- `OPENSWMM_PERF` prints performance counters to the console.
- `OPENSWMM_2D_MARCHER_TELEMETRY` writes solver diagnostics to a local file you name. Despite its name, nothing is transmitted.
- Kokkos profiling variables load a profiling library you choose.
- `HYDROCOUPLE_COMPONENT_PATH` and `OPENSWMM_GPU_PLUGIN_PATH` load plug-in libraries from folders you choose. Third-party plug-ins run with your permissions and are outside this statement.

**When building from source.** The build may download dependencies, for example through vcpkg or from the HydroCouple repository on GitHub. That happens on the build machine, not when the software runs.

---

## 4. Information about contributors

When you contribute to any HydroCouple repository, the project handles the following.

**Your contributions, on GitHub, in public.** This means commits (including the author name and email address your Git client records — you can use a [GitHub-provided no-reply address](https://docs.github.com/account-and-profile/setting-up-and-managing-your-personal-account-on-github/managing-email-preferences/setting-your-commit-email-address)), pull requests, issues, discussions, and comments, together with your GitHub username and public profile. GitHub collects and stores this; we use it to review, accept, and credit contributions.

**Contributor License Agreement records.** Before a contribution can be merged, its author signs the Individual CLA ([CLA.md](./CLA.md)). An organization may also sign the Corporate CLA ([CCLA.md](./CCLA.md)).

- *Signing on GitHub.* Repositories that run the CLA Assistant GitHub Action ([contributor-assistant/github-action](https://github.com/contributor-assistant/github-action)) record your GitHub username, GitHub user ID, the pull request number, and the date and time you signed. The record goes in a file inside that repository (`.github/cla-signatures.json`), so it is **public**. The action runs on GitHub; no other service receives the record. The comment you post to sign is also public on the pull request.
- *Signing by email.* We keep your email, which includes your name, email address, GitHub username, the repository, and the CLA version. We also keep the date we received it and the name of the maintainer who recorded it. The email itself is not published. The public log, [CLA-SIGNATORIES.md](./CLA-SIGNATORIES.md), lists your GitHub username, the CLA version, the date, and the signing method. **It shows your name only if you agree.**
- *Corporate CLA.* We keep the signed agreement, which includes the organization's name and address, the signatory's name and title, and the names, GitHub usernames, and contact email addresses of the employees it designates. The agreement is not published. Whether an organization has a CCLA on file, and which GitHub accounts it covers, may be disclosed where needed to explain why a contribution was accepted.

**Attribution.** If you want to be credited, your name, GitHub username, and any affiliation or ORCID iD you provide may appear in `AUTHORS.md`, `CITATION.cff`, release notes, or documentation.

**Correspondence.** If you email us, we keep the message for as long as needed to deal with it and, where it is a CLA signature or a request under this statement, as a record.

**Why we keep this information, and our legal basis.**

| Information | Purpose | Legal basis (where GDPR / UK GDPR applies) |
|---|---|---|
| Contributions and GitHub identity | Reviewing, accepting, and distributing contributions | Our legitimate interest in running an open-source project, and the contract formed by the CLA |
| CLA and CCLA records | Proving the project has the right to distribute each contribution | Performance of the CLA/CCLA, and our legitimate interest in establishing and defending legal claims |
| Attribution | Crediting your work | Your request, and our legitimate interest in accurate credit |
| Correspondence and requests | Answering you and keeping a record of what was decided | Legitimate interest, and compliance with legal obligations |

We do not use contributor information for marketing, profiling, or automated decision-making.

**How long we keep it.** CLA and CCLA records are kept for as long as any contribution they cover remains part of the project, plus a reasonable period afterwards for limitation purposes. Correspondence that is not a record is deleted when it is no longer needed. Contributions themselves remain in the project's Git history.

**Public means permanent.** Git history, signature files, issues, and pull requests are public and are copied by everyone who clones or forks the repository, and by public archives such as Software Heritage. We can change what the project publishes from now on, but we cannot recall copies that others already hold.

---

## 5. Who receives personal information

- **GitHub, Inc.** hosts the repositories, issues, discussions, the CLA Assistant workflow, and (for the project website) GitHub Pages. GitHub acts under its own privacy statement and is based in the United States.
- **Our email provider** delivers and stores mail sent to the project address.
- **Third-party services you connect the software to** (Section 3) receive what is described there, directly from your device. We are not a party to those exchanges.
- **Authorities**, only where we are legally required to disclose information.
- **A successor steward.** When the HydroCouple Foundation is incorporated, contributor records will be transferred to it. Records may also be transferred to any later successor that takes over stewardship of the project.

We do not sell personal information, share it for cross-context behavioral advertising, or give it to data brokers.

**International transfers.** The project is run from the United States, and GitHub stores data in the United States. If you are outside the United States, your information is transferred there. For information that GitHub holds, GitHub's transfer mechanisms apply, as described in its privacy statement. For information we hold ourselves, the transfer happens because you chose to contribute to or contact a project based in the United States.

---

## 6. Your rights

Wherever you live, you can ask us to:

- **access** the personal information we hold about you, including a copy in a portable format;
- **correct** it;
- **delete** it;
- **restrict** or **object to** how we use it; and
- **withdraw consent** where we relied on it, for example to display your name in CLA-SIGNATORIES.md.

Email [support@hydrocouple.org](mailto:support@hydrocouple.org). We respond within **one month** of receiving your request. If a request is complex, we may extend that, as the law allows, and will tell you why. Requests are free.

**Verification.** Before acting, we confirm that you control the GitHub account or email address the request is about. We ask for nothing beyond that.

**Limits we will explain if they apply:**

- We cannot remove information from copies of public Git history held by others (Section 4).
- We may decline to delete a CLA or CCLA record while the contribution it covers remains in the project, because the record is what allows the project to distribute that contribution. In that case, we can instead remove your name from public display and, at your request, remove or rewrite your contribution going forward.
- Information that GitHub controls, such as your account and profile, is handled through [GitHub's own privacy controls](https://docs.github.com/site-policy/privacy-policies/github-general-privacy-statement).

**Complaints.** You can also complain to a data protection authority, for example the supervisory authority where you live in the EU/EEA or the Information Commissioner's Office in the UK. We would appreciate the chance to resolve your concern first.

---

## 7. Security and incidents

Project records are kept in access-controlled accounts, and access is limited to the people who need it to run the project. If we learn of a security incident affecting personal information we hold, we will assess it promptly. We will notify the people affected, and any authority we are required to notify, within the time the law requires.

To report a security vulnerability in the software itself, email [support@hydrocouple.org](mailto:support@hydrocouple.org) instead of opening a public issue.

---

## 8. Children

The software and the project are not directed at children. We do not knowingly collect personal information from anyone under 16. Signing the CLA requires legal capacity to enter into the agreement.

---

## 9. Changes to this statement

We will update this statement when the software's behavior or the project's practices change, and when the HydroCouple Foundation is incorporated. The version and date at the top tell you when it last changed, and the repository's Git history records every change. If we make a material change that affects contributor information we already hold, we will announce it in the repository before it takes effect.

---

*Copyright © 2026 HydroCouple Foundation. **The HydroCouple Foundation has not yet been formed.** Until it is incorporated and the rights are formally assigned to it, the copyright and other intellectual property the Foundation will hold in this project are held by Caleb Buahin (GitHub: [@cbuahin](https://github.com/cbuahin)), its Executive Director, who holds them on the Foundation's behalf. Contributions from other contributors stay owned by their authors and are licensed to the project under [CLA.md](./CLA.md) and [CCLA.md](./CCLA.md).*
