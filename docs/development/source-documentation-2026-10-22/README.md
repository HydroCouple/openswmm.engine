# Source documentation review — 2026-10-22

The maintained engine source/header and test/helper documentation review is complete in
its working tree: 1,049 files. File Doxygen metadata, Caleb Buahin attribution, original
SPDX/license and upstream public-domain notices, and formatting are checked. Functions,
arguments, results, units, ownership, failure behavior and side effects were documented
from source review, including optional platform and feature branches.

Fresh combined production and test extraction has no strict callable/argument gaps.
Whole-library/default, Windows and Apple/OpenMP scans each retain 22 verified Doxygen
1.17 parser diagnostics: legacy climate definitions inherit static linkage from earlier
prototypes, which Doxygen fails to bind. Complete canonical contracts and internal linkage
were checked against source/compiler evidence. No other warning classes remain; test
extraction is warning-free. These diagnostics are recorded rather than changing C code
solely to satisfy the parser.
Canonical declarations and implementation argument aliases are aligned with explicit
identifier-only ledgers; types/order/ABI and function bodies are preserved. Legacy static
function documentation links and missing module groups are corrected. Exact before-source
snapshots, configured native syntax and focused smoke checks supplement extraction and
formatting checks. This is not a claim that a new full runtime regression suite was run.

For a documentation configuration that defines OPENSWMM_HAS_2D, also add that name to
ENABLED_SECTIONS: two conditional private checkpoint arguments then appear with their
contracts. Official/default base configuration leaves both settings empty.

The initial checkpoint and this final commit together contain the documentation work.
Unrelated pending implementation remains outside these commits. Completed comments tied
to it are preserved in `deferred-source-comments.json`, with hashes, lines and token anchors,
and remain in their fully documented working source files. No implementation is staged
merely to publish its comments.

`validation-summary.json` records final scope and limits. Full local review ledgers,
snapshots, current source manifests and compiler/Doxygen evidence remain under
`tests/verification/function_documentation_2026-10-21/`. Build products, generated Doxygen
output, baseline archives and local workplans are not committed.
