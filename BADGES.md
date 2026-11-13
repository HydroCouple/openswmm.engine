# HydroCouple Badges

Badges celebrate the many particular things that make **openswmm.engine** work: a first merged pull request, a clear bug report, a verification case with documented results, a translation, a discussion that drew a crowd. There are lots of them, they are easy to come by, and no badge ranks above another.

Badges sit alongside the [Contributor Ladder](./LADDER.md) rather than being part of it. **Badges are a celebration only: they carry no permissions and do not count toward ranks.**

The badge names come from stormwater field practice, and many of them happen to describe software practice too. We hope they make you smile.

---

## Contents

1. [How badges work](#how-badges-work)
2. [The badges](#the-badges)
3. [Upstream Credit](#upstream-credit)
4. [For triagers and reviewers](#for-triagers-and-reviewers)
5. [Opting out, corrections, and removal](#opting-out-corrections-and-removal)
6. [Proposing a new badge](#proposing-a-new-badge)

---

## How badges work

**You never need to ask for a badge**, for yourself or for anyone else.

1. **Found.** Most badges are found automatically by the project's recognition bot, from what happens on GitHub: a merged pull request, a discussion that drew replies, or a label a reviewer applied while triaging your issue. A few badges need a human judgement that no label can capture. Those are proposed by project members when they see the work.
2. **Reviewed.** Each week, the bot gathers what it found into a single pull request in the [community repository](https://github.com/HydroCouple/community-recognition), with a link to the work behind every badge. A project Maintainer reviews it and checks the evidence.
3. **Awarded.** Once approved, the badges appear in the [contributors table](https://github.com/HydroCouple/community-recognition/blob/main/CONTRIBUTORS.md), and the bot leaves a short note of congratulations on the pull request, issue, or discussion that earned each one.
4. **Celebrated.** Every badge awarded since the last [Confluence](./LADDER.md#the-confluence) is read out at the next one.

A few details:

- **Each badge is earned once.** Earning it again doesn't add a second.
- **Labels never earn a badge for the person who applied them.**
- **Credit is shared.** For badges earned through a merged pull request, everyone listed as a co-author in its commits is credited along with the author.
- **Badges cover openswmm.engine**, the tutorials about it in [openswmm.lab](https://github.com/HydroCouple/openswmm.lab), and the discussions around them.
- **Badges cannot be bought**, and they are never given in return for sponsorship of the Foundation.

---

## The badges

In the tables below, **How it's found** says whether the bot detects the badge or a project member proposes it. **Key** is the identifier the recognition bot uses.

### Contribution

| Symbol | Badge | Earned by | How it's found | Key |
|---|---|---|---|---|
| 💧 | <a id="first-flush"></a>**First Flush** | Your first merged pull request | Detected | `firstFlush` |
| 🩹 | <a id="cipp"></a>**CIPP** | A merged bug fix *(cured-in-place pipe: a liner that repairs a defect)* | Detected: a merged PR that closes an issue labelled `bug` | `cipp` |
| 🚇 | <a id="trenchless"></a>**Trenchless** | Improving the internals without changing the public API | Detected: a merged PR a reviewer labelled `refactor` | `trenchless` |
| ⛰️ | <a id="peak-flow"></a>**Peak Flow** | The contribution recognised as the most consequential of a release cycle, one per cycle | Proposed by the Executive Director | `peakFlow` |
| 🗓️ | <a id="base-flow"></a>**Base Flow** | Merged contributions in two consecutive `x.y.0` release cycles | Detected | `baseFlow` |
| 🌱 | <a id="green-infrastructure"></a>**Green Infrastructure** | Substantial work on the LID/GI modules | Detected: a merged PR a reviewer labelled `lid-gi` | `greenInfrastructure` |

### Verification & testing

| Symbol | Badge | Earned by | How it's found | Key |
|---|---|---|---|---|
| 🧪 | <a id="dye-test"></a>**Dye Test** | Adding a verification case with documented analytic or field results | Detected: a merged PR that adds a case to the verification corpus | `dyeTest` |
| 💨 | <a id="smoke-test"></a>**Smoke Test** | Filing an issue with a clear, minimal example that reproduces the problem | Detected: an issue a triager labelled `reproducer-confirmed` | `smokeTest` |
| ⚖️ | <a id="continuity-error"></a>**Continuity Error** | Finding a mass-balance or numerical stability bug | Detected: a confirmed bug labelled `mass-balance` or `stability` | `continuityError` |
| 🔍 | <a id="leak-detection"></a>**Leak Detection** | Finding a memory leak | Detected: an issue labelled `memory-leak` | `leakDetection` |
| 🛡️ | <a id="freeboard"></a>**Freeboard** | Catching a regression before it reached a release | Detected: an issue labelled `caught-pre-release` | `freeboard` |
| 📹 | <a id="cctv-crawl"></a>**CCTV Crawl** | Carefully triaging 25 or more open issues in one release cycle | Detected | `cctvCrawl` |

### Documentation & teaching

| Symbol | Badge | Earned by | How it's found | Key |
|---|---|---|---|---|
| 📐 | <a id="as-built"></a>**As-Built** | Bringing the documentation back in line with the code | Detected: a merged PR labelled `docs-fix` | `asBuilt` |
| 📈 | <a id="rating-curve"></a>**Rating Curve** | Writing a tutorial in [openswmm.lab](https://github.com/HydroCouple/openswmm.lab) | Detected | `ratingCurve` |
| 🧭 | <a id="field-guide"></a>**Field Guide** | Writing a complete tutorial series (three or more linked tutorials) | Detected: a completed lab milestone titled "Tutorial series: …" | `fieldGuide` |
| 🌐 | <a id="unit-conversion"></a>**Unit Conversion** | Translation or localisation work | Detected | `unitConversion` |
| 📌 | <a id="datum"></a>**Datum** | A formulation proposal accepted for prototyping ([CONTRIBUTING.md §15](./CONTRIBUTING.md#15-major-project-formulation-changes)) | Detected: a proposal labelled `rfc-accepted` | `datum` |

### Community

| Symbol | Badge | Earned by | How it's found | Key |
|---|---|---|---|---|
| 💬 | <a id="ripple"></a>**Ripple** | Starting a discussion that drew a reply | Detected | `ripple` |
| 🗣️ | <a id="surcharge"></a>**Surcharge** | Starting a discussion that drew 10 or more participants | Detected | `surcharge` |
| 🗺️ | <a id="overflow-event"></a>**Overflow Event** | Starting a discussion that became a roadmap item | Detected: linked from an issue labelled `roadmap` | `overflowEvent` |
| ↩️ | <a id="backwater"></a>**Backwater** | Reviving a stalled thread with something that moved it forward | Proposed by an Interceptor or above | `backwater` |
| ☔ | <a id="wet-weather-response"></a>**Wet Weather Response** | Three or more accepted answers in the week after an `x.y.0` release | Detected | `wetWeatherResponse` |
| 🤝 | <a id="tributary"></a>**Tributary** | Bringing in a new contributor who reached Lateral (they name you as "Referred by" in their first pull request) | Detected | `tributary` |

### Research, use & support

| Symbol | Badge | Earned by | How it's found | Key |
|---|---|---|---|---|
| 🏷️ | <a id="upstream-credit-badge"></a>**Upstream Credit** | Building and publishing a project, product, course, or service on openswmm.engine with proper attribution ([below](#upstream-credit)) | Proposed by a Maintainer | `upstreamCredit` |
| 📄 | <a id="downstream-effect"></a>**Downstream Effect** | Your first publication citing openswmm.engine | Proposed by a Trunk member or above | `downstreamEffect` |
| 🏞️ | <a id="watershed-moment"></a>**Watershed Moment** | A publication in which openswmm.engine is the method, not a tool mentioned in passing | Proposed by an Interceptor or above | `watershedMoment` |
| 🌧️ | <a id="rain-gauge"></a>**Rain Gauge** | Donating a model or dataset to the test corpus | Detected: a merged PR adding a licensed model or dataset | `rainGauge` |

---

## Upstream Credit

In software, *upstream* is the project you build on, and crediting it is how attribution works. In stormwater, a *credit* is what a property earns for managing its runoff well. Upstream Credit recognises people and organisations who build on openswmm.engine and credit it clearly. It pairs with Downstream Effect: that badge is for publications citing the engine, and this one is for things built on it. You don't need to have contributed to the engine to earn it.

**Building on the engine** means linking, embedding, wrapping, bundling, or running openswmm.engine as part of a public project, a product with public documentation, a taught course, or a public service. A single analysis for a report is better recognised by Downstream Effect.

**Proper attribution** means all three of the following:

1. **License:** the engine's license and notice files are kept wherever the engine is distributed.
2. **Name and link:** openswmm.engine is named and linked where your users will see it, such as your README, documentation, About page, or syllabus, not only in a dependency list.
3. **Citation:** wherever you publish results or methods, you cite openswmm.engine as [CITATION.cff](./CITATION.cff) describes.

**Who receives it:** the person who maintains the project, or the GitHub organisation that publishes it.

**How projects are found:** share what you've built in the **Built with openswmm.engine** discussion category, or show it in the open roda at a [Confluence](./LADDER.md#the-confluence). Maintainers also look at GitHub's "Used by" list and at published packages that depend on the engine. Sharing your project is never a badge request; we simply love to see what people build.

**Evidence:** the badge is proposed with public links showing both the use of the engine and the attribution.

Upstream Credit recognises clear attribution. It is not an endorsement or certification of the project, its results, or any product.

---

## For triagers and reviewers

Some badges depend on a judgement that only a person can make. Triagers and reviewers record that judgement with a label, as part of normal triage and review, whenever the work fits a badge's description. Applying labels requires Trunk rank or above.

| Label | Badge | Apply when |
|---|---|---|
| `reproducer-confirmed` | Smoke Test | You reproduced the issue from the reporter's minimal example |
| `mass-balance`, `stability` | Continuity Error | A confirmed `bug` involves conservation or numerical stability |
| `memory-leak` | Leak Detection | A leak is confirmed, with sanitizer or Valgrind evidence |
| `caught-pre-release` | Freeboard | The regression appeared after the last release tag and was caught before the next |
| `refactor` | Trenchless | A merged refactor leaves the public API unchanged |
| `lid-gi` | Green Infrastructure | The PR is substantial LID/GI work |
| `docs-fix` | As-Built | The PR corrects documentation that disagreed with the code |
| `rfc-accepted` | Datum | A formulation proposal is accepted for prototyping (applied by the Executive Director, or by the Chief Scientist for a Designated Subsystem) |
| `roadmap` | Overflow Event | The issue is a roadmap item; link the discussion it came from |

For **Field Guide**, gather a tutorial series' pull requests in an openswmm.lab milestone whose title starts with "Tutorial series:", and close the milestone when the series is complete.

Please don't apply a badge label to your own issue or pull request. The bot ignores it, and another reviewer will remove it.

---

## Opting out, corrections, and removal

- **Opting out.** If you'd rather not appear in the contributors table, open a pull request adding yourself to `awards/optout.toml` in the [community repository](https://github.com/HydroCouple/community-recognition). The bot will stop proposing badges for you.
- **Something missing or wrong?** Please open an issue in the community repository. A missed badge usually means a detection rule needs adjusting, and fixing the rule credits everyone it missed.
- **Removal.** A badge is removed only in the rare case of fabricated evidence or a Code of Conduct violation, by decision of the Executive Director, recorded publicly. Badges record what was done, so they are not removed because circumstances change later.

---

## Proposing a new badge

Contributors at Lateral rank and above are welcome to propose a new badge. Open an issue in the community repository with:

- a name that fits both the stormwater and the software sense, if you can find one,
- what earns it, and
- how it could be found: a GitHub event the bot can detect, a label a reviewer would apply, or the person who would propose it.

The Executive Director decides which badges to add. New badges are added to this page, which is the bot's list of badges.

---

*HydroCouple Badges are maintained by the Executive Director of HydroCouple Foundation. Changes to this page are reviewed by the Executive Director. Last updated: October 2026 (draft).*
