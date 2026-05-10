"""
Control Rule Access
===================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
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

    @ivar _solver: The owning solver instance whose engine handle is used
        for every C call.
    @type _solver: L{Solver}

    @param solver: An active L{Solver} instance. The solver must remain
        alive for the lifetime of this object.
    @type solver: L{Solver}
    """

    def __init__(self, solver):
        """Construct a L{Controls} accessor bound to C{solver}.

        @param solver: An active L{Solver} instance whose engine handle
            will be used for all subsequent control operations.
        @type solver: L{Solver}
        """
        self._solver = solver

    # ====================================================================
    # Rule lookup
    # ====================================================================

    def count(self) -> int:
        """Return the number of control rules in the model.

        @return: Rule count.
        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_control_count(h)

    def get_rule(self, int idx) -> str:
        """Return the text of a control rule by index.

        @param idx: Rule index.
        @type idx: int
        @return: Rule text string.
        @rtype: str
        @raise EngineError: If the underlying C{swmm_control_get_rule} call
            returns a non-zero error code.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef char buf[4096]
        _check(swmm_control_get_rule(h, idx, buf, 4096))
        return buf.decode('utf-8')

    def add_rule(self, str rule_text):
        """Add a control rule to the model.

        @param rule_text: Full rule text (including C{RULE}, C{IF}, and
            C{THEN} clauses).
        @type rule_text: str
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying C{swmm_control_add_rule}
            call returns a non-zero error code (e.g. malformed rule text).
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = rule_text.encode('utf-8')
        _check(swmm_control_add_rule(h, b))

    def clear_rules(self):
        """Remove all control rules from the model.

        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying C{swmm_control_clear_rules}
            call returns a non-zero error code.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_control_clear_rules(h))

    # ====================================================================
    # Direct actions (runtime link overrides)
    # ====================================================================

    def set_link_setting(self, int link_idx, double setting):
        """Override a link's control setting at runtime.

        @param link_idx: Link index.
        @type link_idx: int
        @param setting: New setting value (typically C{0.0}--C{1.0}).
        @type setting: float
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying
            C{swmm_control_set_link_setting} call returns a non-zero
            error code.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_control_set_link_setting(h, link_idx, setting))

    # ====================================================================
    # Rule status
    # ====================================================================

    def set_link_status(self, int link_idx, int status):
        """Override a link's open/closed status at runtime.

        @param link_idx: Link index.
        @type link_idx: int
        @param status: Status code (C{0} = closed, C{1} = open).
        @type status: int
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying
            C{swmm_control_set_link_status} call returns a non-zero
            error code.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_control_set_link_status(h, link_idx, status))
