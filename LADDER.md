# The HydroCouple Contributor Ladder

Welcome! This page explains how contributors to **openswmm.engine** are recognised. There are three ways:

- **The Contributor Ladder:** ranks that grow with your involvement in the project, described on this page.
- **[Badges](./BADGES.md):** frequent, lighthearted recognition for particular contributions.
- **The HydroCouple Awards:** annual awards presented at the Foundation's Annual Community Meeting, also described on this page.

The ladder exists to welcome new contributors and to help them grow into reviewers, verifiers, and Maintainers. Ranks reflect sustained and verifiable work, such as tests, reviews, and benchmark cases with documented results, because careful verification is what makes the engine's results trustworthy.

> **What the ladder covers.** Every role in the project's day-to-day work is open to contributors through the ladder, up to and including Maintainer, with merge authority in the parts of the engine you care for. A few responsibilities belong to the officers of HydroCouple Foundation under its Bylaws, or are assigned by designation: setting technical direction, signing releases, and administering the GitHub organization. These are not reached through the ladder, and we mention them here so the path is clear from the start.

---

## Contents

1. [The ranks](#the-ranks)
2. [How to move up](#how-to-move-up)
3. [Sponsorship](#sponsorship)
4. [What each rank brings](#what-each-rank-brings)
5. [Maintainers](#maintainers)
6. [A voice on the Technical Steering Committee](#a-voice-on-the-technical-steering-committee)
7. [Keeping your rank](#keeping-your-rank)
8. [The Confluence](#the-confluence)
9. [The Annual Community Meeting](#the-annual-community-meeting)
10. [Badges](#badges)
11. [Showing your rank](#showing-your-rank)
12. [Accessibility and language](#accessibility-and-language)

---

## The ranks

The ranks are named after parts of a stormwater collection system. Flow enters at the surface and gathers as it moves downstream, and responsibility grows the same way.

| # | Rank | Colour | What it means | GitHub permission |
|---|---|---|---|---|
| 1 | **Inlet** | ![](https://img.shields.io/badge/-%20-C6DBEF) `#C6DBEF` | You have taken part: opened an issue, joined a discussion, filed a bug, or asked a good question. | Read |
| 2 | **Lateral** | ![](https://img.shields.io/badge/-%20-9ECAE1) `#9ECAE1` | You have had a contribution of any kind merged: documentation, a test, a model, a tutorial, or a translation. | Read |
| 3 | **Trunk** | ![](https://img.shields.io/badge/-%20-6BAED6) `#6BAED6` | You have contributed steadily across at least two release cycles. | Triage |
| 4 | **Interceptor** | ![](https://img.shields.io/badge/-%20-4292C6) `#4292C6` | You care for part of the verification corpus, review others' work, and are asked to review changes in your area. | Triage |
| 5 | **Pump Station** | ![](https://img.shields.io/badge/-%20-2171B5) `#2171B5` | You are a **Maintainer**, with merge authority in your assigned scope. | Write |

The Foundation's officers, the **Executive Director** and the **Chief Scientist**, are appointed under the Foundation's Bylaws rather than through the ladder. The officers and their responsibilities are described in [CONTRIBUTING.md](./CONTRIBUTING.md#1-governance).

Everyone's current rank is listed in [ROSTER.md](./ROSTER.md).

---

## How to move up

Each step has a few criteria. Promotions above Lateral are proposed in a public nomination issue, with links to the work and the names of the sponsors. Anyone can nominate anyone, including themselves. The Executive Director confirms promotions to Trunk and Interceptor, and they are announced at the next [Confluence](#the-confluence).

### Inlet → Lateral

- One merged contribution of any kind. That's all!
- No nomination is needed. You are added to the roster once your contribution is merged.

### Lateral → Trunk

- Contributions merged in **two or more** release cycles.
- At least one contribution that isn't code, such as documentation, a test case, a tutorial, a translation, or a donated model.
- **One sponsor** at Trunk rank or above.

### Trunk → Interceptor

- You are the named owner of at least one verification case, with documented analytic or field results. Verification case owners are assigned by the Chief Scientist.
- You have reviewed at least **five** pull requests by others, with comments that improved the merged result.
- **Two sponsors** at Interceptor rank or above.
- Once per-release DOIs are available, Interceptors are included as authors of each release's DOI.

### Interceptor → Pump Station (Maintainer)

- At least **twelve months** at Interceptor, with continued review activity.
- You own the verification corpus for the subsystem you would maintain, and it passes in the current release.
- **Ten or more** reviews that improved merged results, across at least **two** subsystems.
- **Two sponsors** at Pump Station or above.

Becoming a Maintainer is a designation rather than a promotion: the appointing officer for the scope makes it in writing. That is the Chief Scientist for a [Designated Subsystem](./CONTRIBUTING.md#designated-subsystems) and the Executive Director for everything else. The criteria above describe what they look for. Before designation, a new Maintainer completes a few formalities, including a Conflict of Interest disclosure and confirmation of a personal GitHub account with two-factor authentication ([CONTRIBUTING.md](./CONTRIBUTING.md#maintainers)).

---

## Sponsorship

Every promotion above Lateral needs a sponsor who already holds the rank being sought, or a higher one. The sponsor writes **two sentences** in the nomination issue describing what the person actually did, and those sentences are read aloud when the promotion is announced.

Sponsorship keeps the ladder personal: each rank means that someone who knows your work has vouched for it.

While the ladder is new and few people hold the higher ranks, the Executive Director and the Chief Scientist may act as sponsors at any rank.

---

## What each rank brings

Each rank brings something real, and every rank keeps what the ranks below it bring.

| Rank | What it brings |
|---|---|
| **Inlet** | A place in the [contributors table](https://github.com/HydroCouple/community-recognition/blob/main/CONTRIBUTORS.md) once you earn a badge, and a rank badge for your GitHub profile. |
| **Lateral** | Your name in the release notes. You can also [propose new badges](./BADGES.md#proposing-a-new-badge). |
| **Trunk** | **Triage rights:** labelling, closing, and assigning issues, marking duplicates, and managing discussions. You can sponsor promotions to Trunk. You become a **Recognized Contributor**, able to vote for and stand for the community seat on the Technical Steering Committee. |
| **Interceptor** | Automatic review requests in your area. **Authorship of the release DOI.** You can sponsor promotions to Trunk and Interceptor. |
| **Pump Station** | **Merge authority in your scope.** Recognition as subsystem maintainer in release announcements. You may observe Technical Steering Committee meetings, and you can sponsor promotions at every rank. |

We encourage members of the HydroCouple GitHub organization to make their membership public, so that it shows on their profile.

---

## Maintainers

Maintainers are the people who review, merge, and release changes to the engine. Each Maintainer has a **scope**, such as a subsystem, the documentation, or the build system, set out in their written designation. The current Maintainers and their scopes are listed in [CONTRIBUTING.md](./CONTRIBUTING.md#maintainers).

Within their scope, Maintainers:

- review and approve pull requests from contributors at every rank;
- merge changes once the required approvals and checks have passed;
- prepare pre-release builds; and
- sponsor and mentor contributors on their way up the ladder.

Some responsibilities rest elsewhere, so that each has one clear owner:

- **Releases** are tagged and signed by the Maintainers who hold the infrastructure scope, after the Chief Scientist's verification review ([CONTRIBUTING.md §7](./CONTRIBUTING.md#release-certification)).
- **Organization settings**, such as teams, permissions, branch protection, keys, and integrations, belong to the infrastructure scope.
- **Technical direction** rests with the Executive Director, and with the Chief Scientist within the Designated Subsystems.

The review and merge rules that apply to everyone, including Maintainers, are in [CONTRIBUTING.md §11](./CONTRIBUTING.md#11-pull-request-process).

---

## A voice on the Technical Steering Committee

The Foundation's Technical Steering Committee advises on the engine's priorities and reviews draft standards before they are published. One of its seats is elected by the community.

- **Who votes and who can stand:** all Recognized Contributors, meaning everyone at Trunk rank or above.
- **When:** every two years. A member may serve two terms.
- **How:** the Foundation's Governance and Nominating Committee runs the election and confirms each candidate's eligibility.

---

## Keeping your rank

- **Ranks are permanent.** Once you have earned a rank, it is yours.
- **Permissions are reviewed.** Triage and Write access are reviewed each year, so that access stays with people who are actively using it.
- **Maintainers who step back** are listed as emeritus Maintainers. Their scope passes to its designated successor, and their rank and credit stay as they are. If they return, the appointing officer can designate them again.
- **Removal** of a rank, badge, or award happens only in the rare case of fabricated evidence or a Code of Conduct violation. The decision is recorded publicly.

---

## The Confluence

Twice a year, with each `x.y.0` release (planned for March and September), the community gathers online for **the Confluence**: a short public celebration of the release and of the people behind it. It is livestreamed and recorded so that everyone can join, wherever they are.

| Part | What happens | Time |
|---|---|---|
| 1. Release walkthrough | What shipped, and who built it | 20 min |
| 2. Verification report | What the benchmark corpus tells us about this release | 20 min |
| 3. Promotions | Each promotion is read with its sponsors' two sentences. New Maintainers are presented last, with their scope. | 20 min |
| 4. Badge scroll | Every badge awarded since the last Confluence, read out by name | 15 min |
| 5. Open roda | Anyone can show something they built with openswmm.engine, five minutes each | 15 min |

Promotions are read by an invited guest from outside the project's officers, such as a scientific advisor or a respected member of the stormwater modeling community. Nominations open two weeks before each Confluence, in a public issue. Afterwards, the recording, the promotion list, and the badge scroll are posted for everyone.

Sponsorship of the Foundation has no part in ranks, badges, or awards.

---

## The Annual Community Meeting

Once a year, HydroCouple Foundation holds an open **Annual Community Meeting**. In years with a September release, the meeting includes the September Confluence. It is open to everyone: contributors, users, students, agencies, and anyone curious about the engine. It is held online, livestreamed, and recorded.

| Part | What happens |
|---|---|
| 1. Report to the community | The Executive Director and the Chief Scientist report on the roadmap, releases, verification status, and the Foundation's finances, and answer questions |
| 2. The Confluence | The release walkthrough, verification report, promotions, badge scroll, and open roda described above |
| 3. HydroCouple Awards | The annual awards, described below |
| 4. Community session | Talks from people using openswmm.engine in research, practice, and teaching |

### HydroCouple Awards

The HydroCouple Awards recognise outstanding contributions to openswmm.engine and its community. The awards begin with three:

| Award | Recognises |
|---|---|
| **HydroCouple Emerging Contributor Award** | A contributor in their first year whose work had exceptional impact |
| **HydroCouple Award for Education and Outreach** | Teaching, training materials, translation, mentoring, or welcoming new people into the community |
| **HydroCouple Award for Outstanding Application** | An outstanding project, course, or research tool built on openswmm.engine that credits it properly |

As the community grows, four more will be added. They begin in the first year after 20 or more people hold a rank on the ladder, and are announced a year ahead:

| Award | Recognises |
|---|---|
| **HydroCouple Award for Outstanding Contribution** | The single contribution that most advanced openswmm.engine during the year |
| **HydroCouple Award for Excellence in Verification and Validation** | Rigorous verification work, such as test cases, benchmarks, or reviews, that strengthened confidence in the engine's results |
| **HydroCouple Award for Service to the User Community** | Sustained help to users: answering questions, triaging issues, and making the engine easier to adopt |
| **HydroCouple Distinguished Contributor Award** | Exceptional contribution sustained over many years. Not necessarily given every year |

**How recipients are chosen.** Each year the Executive Director invites a panel of respected people in the field, such as researchers, practitioners, and educators in urban hydrology and stormwater modeling, and names them publicly when nominations open. The panel reviews the nominations, chooses the recipients, writes the citations, and presents the awards at the meeting. The Foundation's officers take no part in choosing recipients and are not eligible for any award. Panel members step aside from any nomination where they have a conflict of interest, such as a close collaborator, a student, or a colleague at the same institution.

**Nominations** open six weeks before the meeting, in a public discussion. Anyone may nominate anyone, including themselves, with a short description of the work.

**An award is given only when it is earned.** In any year, the panel may decide not to present an award, and the award record says so.

**What an award is.** Each recipient receives a certificate, a citation read aloud at the meeting, and a permanent entry in [ROSTER.md](./ROSTER.md#hydrocouple-awards) that anyone can link to and check. Awards are recognition only: they carry no prize money or paid benefits, and they confer no rank or permissions. Award names never change once an award has been given. If you would like to list an award, a suggested form is:

> *HydroCouple Award for Education and Outreach*, HydroCouple Foundation, 2027.

**About the Outstanding Application award.** The award honours the people behind a project and the care they took to build on and credit openswmm.engine. It is not an endorsement or certification of the project, its results, or any product. Projects of the Foundation's Sponsoring Members are not eligible.

---

## Badges

Alongside the ladder, [badges](./BADGES.md) celebrate the many particular things that make the project work: a first merged pull request, a clear bug report, a verification case, a translation, a discussion that drew a crowd. You never need to ask for one. Most are found automatically, and a project Maintainer reviews them each week. Badges are a celebration only: they carry no permissions and do not count toward ranks.

---

## Showing your rank

You are welcome to add your rank badge to your GitHub profile README. Use the line for your rank:

```markdown
[![HydroCouple Inlet](https://img.shields.io/badge/HydroCouple-Inlet-C6DBEF)](https://github.com/HydroCouple/openswmm.engine/blob/develop/ROSTER.md)
[![HydroCouple Lateral](https://img.shields.io/badge/HydroCouple-Lateral-9ECAE1)](https://github.com/HydroCouple/openswmm.engine/blob/develop/ROSTER.md)
[![HydroCouple Trunk](https://img.shields.io/badge/HydroCouple-Trunk-6BAED6)](https://github.com/HydroCouple/openswmm.engine/blob/develop/ROSTER.md)
[![HydroCouple Interceptor](https://img.shields.io/badge/HydroCouple-Interceptor-4292C6)](https://github.com/HydroCouple/openswmm.engine/blob/develop/ROSTER.md)
[![HydroCouple Pump Station](https://img.shields.io/badge/HydroCouple-Pump%20Station-2171B5)](https://github.com/HydroCouple/openswmm.engine/blob/develop/ROSTER.md)
```

Each badge links to [ROSTER.md](./ROSTER.md), where your rank is recorded.

---

## Accessibility and language

- **Colour is never the only signal.** Each rank is always shown with its name and its place in the order, and each badge with its symbol and its name.
- **The rank colours are ordered by lightness**, from the ColorBrewer *Blues* palette, so they read as increasing intensity, like a rainfall map, for readers with colour-vision deficiency and in any language. Text on Inlet, Lateral, and Trunk is dark; on Interceptor and Pump Station it is white.
- **No rank depends on** membership of a professional society, your employer, your country, or any paid credential.
- **Names stay in English; criteria are translated.** The rank and badge names are English wordplay that doesn't translate well, so they stay as they are. We welcome translations of this page into every language the community works in, and translators earn the Unit Conversion badge.

---

*The HydroCouple Contributor Ladder is maintained by the Executive Director of HydroCouple Foundation. Changes to this page are reviewed by the Executive Director. Last updated: October 2026 (draft).*
