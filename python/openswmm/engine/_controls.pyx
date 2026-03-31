"""
Control Rule Access
===================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

The :class:`Controls` class provides access to control rules and runtime
link overrides during a simulation.

.. code-block:: python

    from openswmm.engine import Solver, Controls

    with Solver("model.inp", "model.rpt", "model.out") as s:
        ctrl = Controls(s)
        ctrl.add_rule("RULE MyRule\\nIF NODE J1 DEPTH > 5\\nTHEN PUMP P1 STATUS = ON")
        print(f"Rule count: {ctrl.count()}")
"""

# cython: language_level=3

from ._common cimport *


class Controls:
    """Access control rules and override link settings at runtime.

    :param solver: An active :class:`~openswmm.engine.Solver` instance.
        The solver must remain alive for the lifetime of this object.
    """

    def __init__(self, solver):
        self._solver = solver

    # ------------------------------------------------------------------
    # Rules
    # ------------------------------------------------------------------

    def add_rule(self, str rule_text):
        """Add a control rule to the model.

        :param rule_text: Full rule text (including RULE, IF, THEN clauses).
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = rule_text.encode('utf-8')
        _check(swmm_control_add_rule(h, b))

    def count(self) -> int:
        """Return the number of control rules in the model."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_control_count(h)

    def get_rule(self, int idx) -> str:
        """Return the text of a control rule by index.

        :param idx: Rule index.
        :returns: Rule text string.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef char buf[4096]
        _check(swmm_control_get_rule(h, idx, buf, 4096))
        return buf.decode('utf-8')

    def clear_rules(self):
        """Remove all control rules from the model."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_control_clear_rules(h))

    # ------------------------------------------------------------------
    # Runtime link overrides
    # ------------------------------------------------------------------

    def set_link_setting(self, int link_idx, double setting):
        """Override a link's control setting at runtime.

        :param link_idx: Link index.
        :param setting: New setting value (0.0--1.0).
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_control_set_link_setting(h, link_idx, setting))

    def set_link_status(self, int link_idx, int status):
        """Override a link's open/closed status at runtime.

        :param link_idx: Link index.
        :param status: Status code (0 = closed, 1 = open).
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_control_set_link_status(h, link_idx, status))
