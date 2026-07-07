"""
Control rules (Pythonic v1 surface)
===================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

The :class:`Controls` view, reached via ``solver.controls``, behaves as
a :class:`MutableSequence` of :class:`ControlRule` records over the
``[CONTROLS]`` block, plus the runtime ``set_link_setting`` /
``set_link_status`` direct-action methods.

.. code-block:: python

    with Solver("model.inp") as s:
        # Append a new control rule.
        s.controls.append(\"\"\"
            RULE r1
              IF NODE J1 DEPTH > 1.0
              THEN ORIFICE OR1 SETTING = 0.5
        \"\"\")

        for r in s.controls:
            print(r.id, r.text)

        # Direct runtime actions.
        s.controls.set_link_setting("OR1", 0.25)
        s.controls.set_link_status("OR1", closed=False)
"""

# cython: language_level=3

from collections.abc import MutableSequence
from typing import NamedTuple

from ._common cimport *


cdef inline SWMM_Engine _h(solver):
    return <SWMM_Engine><size_t>solver.handle


cdef inline int _resolve_link(solver, key) except -1:
    return _resolve_index(
        _h(solver), key, swmm_link_index, swmm_link_count, "Link")


class ControlRule(NamedTuple):
    """One row from the ``[CONTROLS]`` block."""
    id: str
    text: str


class Controls(MutableSequence):
    """``solver.controls`` — :class:`MutableSequence` of
    :class:`ControlRule` entries plus runtime link-override helpers."""

    def __init__(self, solver):
        self._solver = solver

    # ---- MutableSequence protocol over the [CONTROLS] block --------

    def __len__(self) -> int:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_control_count(h)

    def __getitem__(self, idx):
        if isinstance(idx, slice):
            return [self[i] for i in range(*idx.indices(len(self)))]
        if not isinstance(idx, int):
            raise TypeError(
                f"Controls index must be int or slice, got {type(idx).__name__}")
        n = len(self)
        if idx < 0:
            idx += n
        if not 0 <= idx < n:
            raise IndexError(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef char id_buf[64]
        cdef char text_buf[4096]
        _check(swmm_control_get_id(h, idx, id_buf, 64))
        _check(swmm_control_get_rule(h, idx, text_buf, 4096))
        return ControlRule(id=id_buf.decode('utf-8'),
                           text=text_buf.decode('utf-8'))

    def __setitem__(self, idx, value):
        # The C API has no per-rule replace; emulate by rebuilding.
        n = len(self)
        if not isinstance(idx, int):
            raise TypeError("Controls index must be int")
        if idx < 0:
            idx += n
        if not 0 <= idx < n:
            raise IndexError(idx)
        existing = [self[i] for i in range(n)]
        existing[idx] = self._unpack(value)
        self.clear()
        for r in existing:
            self.append(r)

    def __delitem__(self, idx):
        if not isinstance(idx, int):
            raise TypeError("Controls index must be int")
        n = len(self)
        if idx < 0:
            idx += n
        if not 0 <= idx < n:
            raise IndexError(idx)
        existing = [self[i] for i in range(n)]
        del existing[idx]
        self.clear()
        for r in existing:
            self.append(r)

    def insert(self, idx, value):
        """Insert *value* at *idx* (emulated via clear + re-add; the C rule API is append-only)."""
        n = len(self)
        if idx < 0:
            idx += n
        idx = max(0, min(idx, n))
        existing = [self[i] for i in range(n)]
        existing.insert(idx, self._unpack(value))
        self.clear()
        for r in existing:
            self.append(r)

    def append(self, value) -> None:
        """Append a control rule parsed from *value* (an INP ``RULE`` block or rule mapping)."""
        text = self._unpack(value)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = text.encode('utf-8')
        _check(swmm_control_add_rule(h, b))

    def clear(self) -> None:
        """Remove all control rules from the model."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_control_clear_rules(h))

    @staticmethod
    def _unpack(value) -> str:
        if isinstance(value, str):
            return value
        if isinstance(value, ControlRule):
            return value.text
        if isinstance(value, dict):
            return value["text"]
        raise TypeError(
            "Controls entry must be a rule text string, a ControlRule, "
            "or a dict with a 'text' key")

    # ---- Runtime direct actions -----------------------------------

    def set_link_setting(self, link, double setting) -> None:
        """Set the control setting for a link by id or index."""
        cdef int idx = _resolve_link(self._solver, link)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_control_set_link_setting(h, idx, setting))

    def set_link_status(self, link, *, bint closed) -> None:
        """Set the open/closed status of a link."""
        cdef int idx = _resolve_link(self._solver, link)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_control_set_link_status(h, idx, 1 if closed else 0))

    # ---- Rule validation ------------------------------------------

    def validate_message(self, str rule_text):
        """Validate a control rule without adding it, returning diagnostics.

        @param rule_text: A full C{[CONTROLS]} rule to check.
        @return: C{(ok, message)} where C{ok} is C{True} when the rule parses
            and C{message} is empty, or C{False} with the engine's
            human-readable error description.
        @rtype: tuple[bool, str]
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = rule_text.encode('utf-8')
        cdef char err_buf[1024]
        err_buf[0] = 0
        cdef int line_out = 0
        cdef int rc = swmm_control_validate_rule(h, b, err_buf, 1024, &line_out)
        return (rc == 0, err_buf.decode('utf-8'))

    def __repr__(self) -> str:
        try:
            return f"<Controls n={len(self)}>"
        except Exception:
            return "<Controls (engine closed)>"
