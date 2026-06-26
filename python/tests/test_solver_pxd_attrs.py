"""Solver cdef-class attribute drift test.

``Solver`` is a ``cdef class`` whose attribute slots are declared in
``_solver.pxd``. Assigning an undeclared attribute (``self._foo = ...``)
in ``_solver.pyx`` is **not** a Cython compile error — it compiles into a
generic Python attribute set that fails at runtime with
``AttributeError: 'Solver' object has no attribute '_foo'`` on every
instance, because cdef-class instances carry no ``__dict__``.

This is a pure-text check (no compiled extension needed) so the mistake
is caught before a wheel is ever built. It exists because exactly this
regression shipped once: ``_surface2d`` was assigned in ``__init__``
without a ``cdef object _surface2d`` slot, breaking ``Solver(...)`` for
every model (2026-06-11).
"""

from __future__ import annotations

import re
from pathlib import Path

_ENGINE_DIR = Path(__file__).resolve().parent.parent / "openswmm" / "engine"
_PYX = _ENGINE_DIR / "_solver.pyx"
_PXD = _ENGINE_DIR / "_solver.pxd"


def _solver_class_body(pyx_source: str) -> str:
    """Return the source of the ``cdef class Solver:`` block only.

    The block ends at the next module-level (column-0) statement.
    """
    match = re.search(r"^cdef class Solver\b.*?:\s*$", pyx_source, flags=re.M)
    assert match, "cdef class Solver not found in _solver.pyx"
    start = match.end()
    tail = pyx_source[start:]
    end = re.search(r"^\S", tail, flags=re.M)
    return tail[: end.start()] if end else tail


def _assigned_attrs(class_body: str) -> set[str]:
    """Every ``self._name`` that is the target of an assignment."""
    return set(re.findall(r"\bself\.(_[a-z]\w*)\s*=[^=]", class_body))


def _declared_attrs(pxd_source: str) -> set[str]:
    """Every attribute slot declared in the pxd's Solver block."""
    match = re.search(r"^cdef class Solver\b.*?:\s*$", pxd_source, flags=re.M)
    assert match, "cdef class Solver not found in _solver.pxd"
    body = pxd_source[match.end() :]
    end = re.search(r"^\S", body, flags=re.M)
    if end:
        body = body[: end.start()]
    # ``cdef <type...> name1, name2`` — take every identifier after the
    # type, comma-separated.
    declared: set[str] = set()
    for line in body.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line.startswith("cdef "):
            continue
        names = re.findall(r"(_[a-z]\w*)\s*(?:,|$)", line)
        declared.update(names)
    return declared


class TestSolverPxdAttrs:
    def test_every_assigned_attr_is_declared(self):
        assigned = _assigned_attrs(_solver_class_body(_PYX.read_text()))
        declared = _declared_attrs(_PXD.read_text())
        missing = sorted(assigned - declared)
        assert not missing, (
            f"{len(missing)} attribute(s) assigned on Solver in _solver.pyx "
            f"but not declared in _solver.pxd: {missing}\n\n"
            "Add a 'cdef object <name>' (or typed) slot to the Solver block "
            "in _solver.pxd — undeclared assignments compile but raise "
            "AttributeError at runtime on every Solver instance."
        )

    def test_sanity_known_attrs_found(self):
        # Guard the parser itself: these have been in both files for a while.
        assigned = _assigned_attrs(_solver_class_body(_PYX.read_text()))
        declared = _declared_attrs(_PXD.read_text())
        for name in ("_handle", "_options", "_userflags", "_surface2d"):
            assert name in assigned, f"parser failed to see assignment of {name}"
            assert name in declared, f"parser failed to see declaration of {name}"
