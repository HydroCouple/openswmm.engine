"""C API ↔ Cython binding coverage drift test.

Per :file:`docs/C_API_BINDINGS_MCP_IMPROVEMENT_PLAN.md` §Phase 6.1 — assert
that every ``SWMM_ENGINE_API`` symbol declared in
``include/openswmm/engine/*.h`` has a matching ``cdef extern`` (or other
direct reference) in the Cython ``.pxd`` / ``.pyx`` files under
``python/openswmm/engine/``.

The test does **not** import the compiled extension — it is a pure-text
analysis that runs even in environments where the C extension has not
been built (CI source-only jobs, IDE linting, etc.).

If you intentionally add a new C symbol that does not need a Python
binding (e.g. an experimental / internal helper), add its name to
``KNOWN_UNBOUND`` below with a one-line justification.  The set's only
job is to prevent **accidental** binding gaps — every entry here is a
conscious choice, not a TODO.
"""

from __future__ import annotations

import os
import re
from pathlib import Path

import pytest


# ---------------------------------------------------------------------------
# Repository layout
# ---------------------------------------------------------------------------
# This file lives at ``python/tests/test_api_coverage.py``.  The C headers
# live at ``include/openswmm/engine/*.h`` two directories up; the Cython
# sources at ``python/openswmm/engine/*.{pxd,pyx}``.

_TESTS_DIR = Path(__file__).resolve().parent
_PYTHON_DIR = _TESTS_DIR.parent
_REPO_ROOT = _PYTHON_DIR.parent

_HEADER_GLOB = _REPO_ROOT / "include" / "openswmm" / "engine"
_CYTHON_DIR = _PYTHON_DIR / "openswmm" / "engine"


# ---------------------------------------------------------------------------
# Allowlist of symbols intentionally not bound by Cython (Phase 6.1 baseline).
#
# Each entry is justified.  When binding any of these, just remove the line —
# the test will then enforce that the binding stays in place.  When adding
# brand-new C symbols that should be bound, the test will fail until you
# either add the binding or extend this allowlist (with justification).
# ---------------------------------------------------------------------------
KNOWN_UNBOUND: frozenset[str] = frozenset({
    # (empty as of 2026-06-10 — the full C API surface is bound; see
    # docs/API_GAP_CLOSURE_PLAN_2026-06-10.md.  Add entries here, with a
    # one-line justification, only for symbols that intentionally stay
    # unbound.)
})


# Regex to extract C API symbol names from header declarations.  Handles
# the common multi-line shape ``SWMM_ENGINE_API <return-type> <name>(``
# where ``<return-type>`` may span multiple identifiers (e.g.
# ``SWMM_ENGINE_API const char* swmm_x(``) and may be followed by
# whitespace, newlines, or a ``*``.
_C_API_PATTERN = re.compile(
    r"SWMM_ENGINE_API\s+(?:\w+\s+)+\*?\s*(swmm_[a-z_0-9]+)\s*\(",
    re.MULTILINE,
)

# Any reference to a ``swmm_*`` identifier in Cython files is treated as a
# binding — we don't try to distinguish ``cdef extern`` declarations from
# call sites because the latter implies the former somewhere upstream.
_CYTHON_REF_PATTERN = re.compile(r"\b(swmm_[a-z_0-9]+)\b")


def _collect_c_symbols() -> set[str]:
    """Return every C API symbol declared with ``SWMM_ENGINE_API``."""
    assert _HEADER_GLOB.is_dir(), (
        f"Header directory not found: {_HEADER_GLOB}.  Has the project "
        f"layout changed?")
    symbols: set[str] = set()
    for header in sorted(_HEADER_GLOB.glob("*.h")):
        src = header.read_text(encoding="utf-8")
        for m in _C_API_PATTERN.finditer(src):
            symbols.add(m.group(1))
    return symbols


def _collect_cython_refs() -> set[str]:
    """Return every ``swmm_*`` identifier referenced in Cython files."""
    assert _CYTHON_DIR.is_dir(), (
        f"Cython source directory not found: {_CYTHON_DIR}.")
    refs: set[str] = set()
    for path in sorted(_CYTHON_DIR.glob("*.pxd")):
        src = path.read_text(encoding="utf-8")
        for m in _CYTHON_REF_PATTERN.finditer(src):
            refs.add(m.group(1))
    for path in sorted(_CYTHON_DIR.glob("*.pyx")):
        src = path.read_text(encoding="utf-8")
        for m in _CYTHON_REF_PATTERN.finditer(src):
            refs.add(m.group(1))
    return refs


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


class TestApiCoverage:
    """Drift guards for the C API ↔ Cython binding surface."""

    def test_extraction_finds_a_large_surface(self):
        """Sanity: if either side returns near-zero symbols the regexes
        have broken and the rest of this file is silently meaningless."""
        c_symbols = _collect_c_symbols()
        cython_refs = _collect_cython_refs()
        # Soft floors — the project has hundreds of API symbols.  These
        # numbers are well below current counts (~626 C / ~593 Cython at
        # time of writing) so a routine refactor won't trip them, but a
        # broken regex extracting 0 or 5 symbols will.
        assert len(c_symbols) > 200, (
            f"Only {len(c_symbols)} C API symbols extracted — regex broken?")
        assert len(cython_refs) > 200, (
            f"Only {len(cython_refs)} Cython refs extracted — regex broken?")

    def test_every_c_symbol_is_bound_or_allowlisted(self):
        """The headline drift assertion.

        Every ``SWMM_ENGINE_API`` symbol must either be referenced from a
        Cython ``.pxd`` / ``.pyx`` file (i.e. callable from Python) or be
        explicitly listed in ``KNOWN_UNBOUND`` with a justification.
        """
        c_symbols = _collect_c_symbols()
        cython_refs = _collect_cython_refs()
        unbound = c_symbols - cython_refs - KNOWN_UNBOUND
        if unbound:
            joined = "\n  - " + "\n  - ".join(sorted(unbound))
            pytest.fail(
                f"{len(unbound)} new C API symbol(s) are not bound by "
                f"Cython and are not in the allowlist:{joined}\n\n"
                "Either add a `cdef extern` declaration in "
                "`python/openswmm/engine/_common.pxd` (or the appropriate "
                ".pxd / .pyx), or extend `KNOWN_UNBOUND` in "
                "test_api_coverage.py with a one-line justification.")

    def test_allowlist_does_not_contain_phantoms(self):
        """If an allowlisted symbol no longer exists in the headers (e.g.
        it was renamed or removed), the test should flag it so the
        allowlist stays clean."""
        c_symbols = _collect_c_symbols()
        phantoms = KNOWN_UNBOUND - c_symbols
        if phantoms:
            joined = "\n  - " + "\n  - ".join(sorted(phantoms))
            pytest.fail(
                f"{len(phantoms)} allowlisted symbol(s) do not exist in "
                f"any header — please remove them from "
                f"``KNOWN_UNBOUND``:{joined}")

    def test_allowlist_does_not_shadow_bound_symbols(self):
        """If a symbol on the allowlist actually IS bound, the allowlist
        entry is misleading — flag it so the entry is removed.

        This catches the case where someone binds a symbol but forgets to
        remove its allowlist entry."""
        cython_refs = _collect_cython_refs()
        shadowed = KNOWN_UNBOUND & cython_refs
        if shadowed:
            joined = "\n  - " + "\n  - ".join(sorted(shadowed))
            pytest.fail(
                f"{len(shadowed)} symbol(s) on the allowlist are actually "
                f"bound — please remove them from ``KNOWN_UNBOUND``:"
                f"{joined}")


# ---------------------------------------------------------------------------
# A diagnostic that's useful to inspect manually:
#
#   python -m pytest python/tests/test_api_coverage.py::test_print_summary -s
#
# ...prints how many symbols on each side and how many are bound.  Not a
# regression assertion; just a friendly diagnostic.
# ---------------------------------------------------------------------------


def test_print_summary(capsys):
    """Diagnostic dump (always passes)."""
    c_symbols = _collect_c_symbols()
    cython_refs = _collect_cython_refs()
    intersect = c_symbols & cython_refs
    unbound = c_symbols - cython_refs
    unjustified = unbound - KNOWN_UNBOUND
    with capsys.disabled():
        print()
        print(f"  C API symbols (SWMM_ENGINE_API): {len(c_symbols):>4}")
        print(f"  Cython references:               {len(cython_refs):>4}")
        print(f"  Bound (intersection):            {len(intersect):>4}")
        print(f"  Unbound total:                   {len(unbound):>4}")
        print(f"    of which allowlisted:          {len(KNOWN_UNBOUND):>4}")
        print(f"    of which unjustified:          {len(unjustified):>4}")
