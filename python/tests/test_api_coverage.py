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
    # ---- Aquifer editor (3) -------------------------------------------------
    # GW aquifer table management surfaces; no Python consumers yet — GW
    # users edit via INP for now.  Track as a Phase-3-style binding batch.
    "swmm_aquifer_add",
    "swmm_aquifer_count",
    "swmm_aquifer_index",

    # ---- Control rule validation (1) ----------------------------------------
    # Standalone validator for the rule mini-language.  The Python
    # ``Controls`` class exposes ``add_rule`` which already validates
    # implicitly; standalone validation is a niche feature.
    "swmm_control_validate_rule",

    # ---- DWF / external-inflow editors (4) ----------------------------------
    # Read/remove half of the DWF and ExtInflow editor surface.  Python
    # bindings expose ``add`` and ``count`` for both; ``get`` / ``remove``
    # are tracked as a follow-up binding batch.
    "swmm_dwf_get",
    "swmm_dwf_remove",
    "swmm_ext_inflow_get",
    "swmm_ext_inflow_remove",

    # ---- Hydrograph (RDII) editor (7) ---------------------------------------
    # The Python ``Inflows`` class binds the add/count surface; the
    # finer-grained edit/remove API is a follow-up.
    "swmm_hydrograph_clear_group_months",
    "swmm_hydrograph_group_rename",
    "swmm_hydrograph_remove_entry",
    "swmm_hydrograph_remove_group",
    "swmm_hydrograph_set_gage",
    "swmm_hydrograph_set_ia",
    "swmm_hydrograph_set_rtk",

    # ---- By-name index lookups (3) ------------------------------------------
    # The Python classes for inlets / LIDs / streets expose ``add`` /
    # ``count`` but not ``index``.  Adding ``get_index(name)`` is a thin
    # follow-up.
    "swmm_inlet_index",
    "swmm_lid_index",
    "swmm_street_index",

    # ---- Tag editors (6) ----------------------------------------------------
    # [TAGS] section round-trip is implemented at the engine level but the
    # Python classes do not yet surface get/set methods.  Useful for GUI
    # builders; tracked as a follow-up binding batch.
    "swmm_link_get_tag",
    "swmm_link_set_tag",
    "swmm_node_get_tag",
    "swmm_node_set_tag",
    "swmm_subcatch_get_tag",
    "swmm_subcatch_set_tag",

    # ---- Outfall stage-data readers (2) -------------------------------------
    # The setters for tidal / timeseries outfall stages are bound; the
    # corresponding readers are tested at the C++ level but the Python
    # ``Nodes`` class does not surface them yet.
    "swmm_node_get_outfall_tidal",
    "swmm_node_get_outfall_timeseries",

    # ---- Pattern editor (6) -------------------------------------------------
    # The Python ``Tables`` class binds ``pattern_add`` /
    # ``pattern_count`` / ``pattern_set_factors``; the per-element
    # accessors and remove/rename surface are a follow-up.
    "swmm_pattern_get_factor",
    "swmm_pattern_get_factor_count",
    "swmm_pattern_get_type",
    "swmm_pattern_index",
    "swmm_pattern_remove",
    "swmm_pattern_rename",

    # ---- RDII per-element remove/set (3) ------------------------------------
    "swmm_rdii_decay_remove",
    "swmm_rdii_decay_set",
    "swmm_rdii_remove",

    # ---- Snowpack editor (3) ------------------------------------------------
    # Snow state is currently a placeholder (see audit Appendix on
    # ``swmm_subcatch_get_snow_depth``); the snowpack editor will land
    # alongside full snow-state integration.
    "swmm_snowpack_add",
    "swmm_snowpack_count",
    "swmm_snowpack_index",

    # ---- Table type query (1) -----------------------------------------------
    # ``Tables`` binds the value-level get/set surface; the meta-level
    # ``get_type`` is a one-line follow-up.
    "swmm_table_get_type",

    # ---- Transect editor (15) -----------------------------------------------
    # Read-back / remove / rename of the [TRANSECTS] section.  The Python
    # ``Infrastructure`` class binds ``add_transect`` /
    # ``add_transect_station`` / ``count`` / ``set_*_params``; the rest of
    # the editor surface is queued as a follow-up batch.
    "swmm_transect_clear_stations",
    "swmm_transect_get_bank_stations",
    "swmm_transect_get_comments",
    "swmm_transect_get_encroachment_stations",
    "swmm_transect_get_modifiers",
    "swmm_transect_get_roughness",
    "swmm_transect_get_station",
    "swmm_transect_get_station_count",
    "swmm_transect_index",
    "swmm_transect_remove",
    "swmm_transect_rename",
    "swmm_transect_set_bank_stations",
    "swmm_transect_set_comments",
    "swmm_transect_set_encroachment_stations",
    "swmm_transect_set_modifiers",
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
