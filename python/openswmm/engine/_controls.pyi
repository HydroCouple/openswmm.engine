"""
Control Rule Access
===================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

Type stubs for :mod:`openswmm.engine._controls`.

The :class:`Controls` class provides access to control rules and runtime
link overrides during a simulation.
"""

from ._solver import Solver


class Controls:
    """Access control rules and override link settings at runtime.

    Args:
        solver: An active :class:`Solver` instance. The solver must remain
                alive for the lifetime of this object.

    Example::

        from openswmm.engine import Solver, Controls

        with Solver("model.inp", "model.rpt", "model.out") as s:
            ctrl = Controls(s)
            ctrl.add_rule("RULE R1\\nIF NODE J1 DEPTH > 5\\nTHEN PUMP P1 STATUS = ON")
            print(f"Rule count: {ctrl.count()}")
    """

    def __init__(self, solver: Solver) -> None: ...

    # ------------------------------------------------------------------
    # Rules
    # ------------------------------------------------------------------

    def add_rule(self, rule_text: str) -> None:
        """Add a control rule to the model.

        Args:
            rule_text: Full rule text including RULE, IF, and THEN clauses.
        """
        ...

    def count(self) -> int:
        """Return the number of control rules in the model.

        Returns:
            Rule count.
        """
        ...

    def get_rule(self, idx: int) -> str:
        """Return the text of a control rule by index.

        Args:
            idx: Rule index.

        Returns:
            Rule text string.
        """
        ...

    def clear_rules(self) -> None:
        """Remove all control rules from the model."""
        ...

    # ------------------------------------------------------------------
    # Runtime link overrides
    # ------------------------------------------------------------------

    def set_link_setting(self, link_idx: int, setting: float) -> None:
        """Override a link's control setting at runtime.

        Args:
            link_idx: Link index.
            setting: New setting value (0.0–1.0).
        """
        ...

    def set_link_status(self, link_idx: int, status: int) -> None:
        """Override a link's open/closed status at runtime.

        Args:
            link_idx: Link index.
            status: Status code (0 = closed, 1 = open).
        """
        ...
