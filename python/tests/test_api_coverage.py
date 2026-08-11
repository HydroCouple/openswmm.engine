"""C API ↔ Cython binding coverage drift test.

Per :file:`docs/C_API_BINDINGS_MCP_IMPROVEMENT_PLAN.md` §Phase 6.1 — assert
that every ``SWMM_ENGINE_API`` symbol declared in
``include/openswmm/engine/*.h`` is actually **used** (called, or passed as a
function pointer) from the Cython ``.pyx`` sources under
``python/openswmm/engine/``.

Coverage semantics (tightened 2026-07-06)
-----------------------------------------
A symbol counts as *bound* only when it appears in a ``.pyx`` **outside** of a
``cdef extern`` declaration block and outside of comments — i.e. there is a
real call site or function-pointer reference in the Python-facing layer.

This is deliberately stricter than "referenced anywhere in ``.pxd`` / ``.pyx``":
a symbol declared ``cdef extern`` in a ``.pxd`` (or in a ``.pyx`` extern block)
but never actually invoked is **not** reachable from Python, and the older
reference-anywhere rule would silently pass it. (This is exactly how
``swmm_get_current_time`` slipped through — declared in ``_common.pxd`` but
never called; the capability is served by ``Solver.current_datetime``.)

The test does **not** import the compiled extension — it is a pure-text
analysis that runs even in environments where the C extension has not
been built (CI source-only jobs, IDE linting, etc.).

If you intentionally add a new C symbol that does not need a Python
binding (e.g. an experimental / internal helper, or a facility superseded
by a Python idiom), add its name to ``KNOWN_UNBOUND`` below with a one-line
justification.  The set's only job is to prevent **accidental** binding gaps
— every entry here is a conscious choice, not a TODO.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


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
# Allowlist of symbols intentionally not *used* by Cython.
#
# Each entry is justified.  When binding any of these, just remove the line —
# the test will then enforce that the binding stays in place.  When adding
# brand-new C symbols that should be bound, the test will fail until you
# either add the binding or extend this allowlist (with justification).
#
# Keep this in sync with the ``intentional`` rows in
# ``plans/parity/overrides.tsv``.
# ---------------------------------------------------------------------------
KNOWN_UNBOUND: frozenset[str] = frozenset({
    # Error introspection — Python raises a typed ``EngineError`` instead of
    # polling C-level last-error state, so these are never called directly.
    # (The lenient-open accumulator API — swmm_get_error_count/at and
    # swmm_get_warning_count/at — *is* bound, via ``Solver.open_errors`` /
    # ``Solver.open_warnings``; only the single-shot last-error accessors and
    # the static code→string lookup remain served by the exception layer.)
    "swmm_error_message",
    "swmm_get_last_error",
    "swmm_get_last_error_msg",
    # Current simulation time — declared in _common.pxd but intentionally not
    # called; ``Solver.current_datetime`` derives it as start_datetime +
    # elapsed (the C func returns elapsed seconds, not an OADate).
    "swmm_get_current_time",
})


# Regex to extract C API symbol names from header declarations.  A declaration
# begins with the ``SWMM_ENGINE_API`` export macro and may span multiple lines
# up to the terminating ``;``.  Matching the ``swmm_xxx(`` token on the joined
# declaration correctly captures pointer-returning functions such as
# ``SWMM_ENGINE_API const char* swmm_error_message(int code);`` that a
# return-type-anchored pattern would miss.
_EXPORT_PREFIX = "SWMM_ENGINE_API"
_C_FUNC_TOKEN = re.compile(r"\b(swmm_[a-z0-9_]+)\s*\(")

# Any ``swmm_*`` identifier.
_SWMM_TOKEN = re.compile(r"\b(swmm_[a-z0-9_]+)\b")

# Start of a ``cdef extern`` declaration block (its body declares — does not
# call — C symbols, so it must be excluded from the "used" scan).
_CDEF_EXTERN = re.compile(r"cdef\s+extern\b")


def _collect_c_symbols() -> set[str]:
    """Return every C API symbol declared with ``SWMM_ENGINE_API``."""
    assert _HEADER_GLOB.is_dir(), (
        f"Header directory not found: {_HEADER_GLOB}.  Has the project "
        f"layout changed?")
    symbols: set[str] = set()
    for header in sorted(_HEADER_GLOB.glob("*.h")):
        if header.name.endswith("_export.h"):
            continue
        lines = header.read_text(encoding="utf-8").splitlines()
        i = 0
        while i < len(lines):
            if lines[i].lstrip().startswith(_EXPORT_PREFIX):
                buf = [lines[i]]
                while ";" not in lines[i] and i + 1 < len(lines):
                    i += 1
                    buf.append(lines[i])
                joined = re.sub(r"\s+", " ", " ".join(buf))
                m = _C_FUNC_TOKEN.search(joined)
                if m:
                    symbols.add(m.group(1))
            i += 1
    return symbols


def _collect_pyx_uses() -> set[str]:
    """Return every ``swmm_*`` symbol *used* from ``.pyx`` sources.

    "Used" = referenced outside of ``cdef extern`` declaration blocks and
    outside of ``#`` comments — i.e. an actual call site or function-pointer
    reference in the Python-facing layer.
    """
    assert _CYTHON_DIR.is_dir(), (
        f"Cython source directory not found: {_CYTHON_DIR}.")
    uses: set[str] = set()
    for path in sorted(_CYTHON_DIR.glob("*.pyx")):
        in_extern = False
        extern_indent = 0
        for raw in path.read_text(encoding="utf-8").splitlines():
            code = raw.split("#", 1)[0]           # drop line comments
            stripped = code.strip()
            if _CDEF_EXTERN.match(stripped):
                in_extern = True
                extern_indent = len(code) - len(code.lstrip())
                continue
            if in_extern:
                # extern block ends at the next non-blank line whose indent
                # returns to <= the block-header indent.
                if stripped and (len(code) - len(code.lstrip())) <= extern_indent:
                    in_extern = False
                else:
                    continue
            for m in _SWMM_TOKEN.finditer(code):
                uses.add(m.group(1))
    return uses


def _collect_pxd_decls() -> set[str]:
    """Return every ``swmm_*`` symbol declared in ``.pxd`` extern blocks."""
    decls: set[str] = set()
    for path in sorted(_CYTHON_DIR.glob("*.pxd")):
        for m in _SWMM_TOKEN.finditer(path.read_text(encoding="utf-8")):
            decls.add(m.group(1))
    return decls


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


class TestApiCoverage(unittest.TestCase):
    """Drift guards for the C API ↔ Cython binding surface."""

    def test_extraction_finds_a_large_surface(self):
        """Sanity: if either side returns near-zero symbols the regexes
        have broken and the rest of this file is silently meaningless."""
        c_symbols = _collect_c_symbols()
        pyx_uses = _collect_pyx_uses()
        # Soft floors — the project has hundreds of API symbols.  These
        # numbers are well below current counts (~823 C / ~819 used at time
        # of writing) so a routine refactor won't trip them, but a broken
        # regex extracting 0 or 5 symbols will.
        self.assertGreater(len(c_symbols), 400, (
            f"Only {len(c_symbols)} C API symbols extracted — regex broken?"))
        self.assertGreater(len(pyx_uses), 400, (
            f"Only {len(pyx_uses)} Cython uses extracted — regex broken?"))

    def test_every_c_symbol_is_used_or_allowlisted(self):
        """The headline drift assertion.

        Every ``SWMM_ENGINE_API`` symbol must either have a real use site in
        a ``.pyx`` (call or function-pointer reference) or be explicitly
        listed in ``KNOWN_UNBOUND`` with a justification.

        A ``cdef extern`` declaration alone does **not** count — the symbol
        must actually be invoked.
        """
        c_symbols = _collect_c_symbols()
        pyx_uses = _collect_pyx_uses()
        unbound = c_symbols - pyx_uses - KNOWN_UNBOUND
        if unbound:
            joined = "\n  - " + "\n  - ".join(sorted(unbound))
            self.fail(
                f"{len(unbound)} C API symbol(s) are declared but never "
                f"called from any `.pyx` and are not in the allowlist:{joined}"
                "\n\nEither add a call site in the appropriate `.pyx` wrapper "
                "(a `cdef extern` declaration in `_common.pxd` is not enough "
                "on its own), or extend `KNOWN_UNBOUND` in "
                "test_api_coverage.py with a one-line justification.")

    def test_allowlist_does_not_contain_phantoms(self):
        """If an allowlisted symbol no longer exists in the headers (e.g.
        it was renamed or removed), the test should flag it so the
        allowlist stays clean."""
        c_symbols = _collect_c_symbols()
        phantoms = KNOWN_UNBOUND - c_symbols
        if phantoms:
            joined = "\n  - " + "\n  - ".join(sorted(phantoms))
            self.fail(
                f"{len(phantoms)} allowlisted symbol(s) do not exist in "
                f"any header — please remove them from "
                f"``KNOWN_UNBOUND``:{joined}")

    def test_allowlist_does_not_shadow_used_symbols(self):
        """If a symbol on the allowlist actually IS used, the allowlist
        entry is misleading — flag it so the entry is removed.

        This catches the case where someone binds a symbol but forgets to
        remove its allowlist entry."""
        pyx_uses = _collect_pyx_uses()
        shadowed = KNOWN_UNBOUND & pyx_uses
        if shadowed:
            joined = "\n  - " + "\n  - ".join(sorted(shadowed))
            self.fail(
                f"{len(shadowed)} symbol(s) on the allowlist are actually "
                f"used — please remove them from ``KNOWN_UNBOUND``:"
                f"{joined}")


# ---------------------------------------------------------------------------
# A diagnostic that's useful to inspect manually:
#
#   python -m unittest tests.test_api_coverage.TestPrintSummary -v
#
# ...prints how many symbols on each side and how many are used.  Not a
# regression assertion; just a friendly diagnostic.  It also surfaces any
# ``.pxd``-declared-but-never-called symbols, which are the class of latent
# gap the tightened rule now catches.
# ---------------------------------------------------------------------------


class TestPrintSummary(unittest.TestCase):
    def test_print_summary(self):
        """Diagnostic dump (always passes)."""
        c_symbols = _collect_c_symbols()
        pyx_uses = _collect_pyx_uses()
        pxd_decls = _collect_pxd_decls()
        used = c_symbols & pyx_uses
        unbound = c_symbols - pyx_uses
        unjustified = unbound - KNOWN_UNBOUND
        declared_not_called = (c_symbols & pxd_decls) - pyx_uses
        print()
        print(f"  C API symbols (SWMM_ENGINE_API): {len(c_symbols):>4}")
        print(f"  Cython .pyx uses:                {len(pyx_uses):>4}")
        print(f"  Used (intersection):             {len(used):>4}")
        print(f"  Unbound total:                   {len(unbound):>4}")
        print(f"    of which allowlisted:          {len(KNOWN_UNBOUND):>4}")
        print(f"    of which unjustified:          {len(unjustified):>4}")
        print(f"  Declared in .pxd but not called: {len(declared_not_called):>4}")
