"""
Pollutant Access
================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

Type stubs for :mod:`openswmm.engine._pollutants`.

The :class:`Pollutants` class provides access to pollutant properties and
runtime water-quality injection during a simulation.
"""

from typing import Union

from ._solver import Solver


class Pollutants:
    """Access pollutant properties and inject quality at nodes and links.

    All per-element methods accept either an integer index or a string
    pollutant ID. When a string is passed it is resolved via
    :meth:`get_index`.

    Args:
        solver: An active :class:`Solver` instance. The solver must remain
                alive for the lifetime of this object.

    Example::

        from openswmm.engine import Solver, Pollutants

        with Solver("model.inp", "model.rpt", "model.out") as s:
            poll = Pollutants(s)
            print(f"Pollutant count: {poll.count()}")
            while s.step():
                conc = poll.get_init_conc("TSS")
                poll.set_node_quality("J1", 0, 12.5)
    """

    def __init__(self, solver: Solver) -> None: ...

    def _resolve(self, idx: Union[int, str]) -> int:
        """Resolve *idx* to an integer pollutant index.

        Args:
            idx: Integer index or string pollutant ID.

        Returns:
            Integer index.

        Raises:
            KeyError: If a string ID is not found.
        """
        ...

    # ------------------------------------------------------------------
    # Identity
    # ------------------------------------------------------------------

    def count(self) -> int:
        """Return the number of pollutants in the model.

        Returns:
            Pollutant count.
        """
        ...

    def get_index(self, pollut_id: str) -> int:
        """Return the integer index of a pollutant by its string ID.

        Args:
            pollut_id: Pollutant identifier string.

        Returns:
            Pollutant index, or -1 if not found.
        """
        ...

    def get_id(self, idx: int) -> str:
        """Return the string ID of a pollutant by index.

        Args:
            idx: Pollutant index.

        Returns:
            Pollutant ID string.
        """
        ...

    # ------------------------------------------------------------------
    # Property getters
    # ------------------------------------------------------------------

    def get_units(self, idx: Union[int, str]) -> int:
        """Return the concentration units code for a pollutant.

        Args:
            idx: Pollutant index (int) or ID (str).

        Returns:
            Units code (see :class:`~openswmm.engine.ConcentrationUnits`).
        """
        ...

    def get_kdecay(self, idx: Union[int, str]) -> float:
        """Return the first-order decay coefficient.

        Args:
            idx: Pollutant index (int) or ID (str).

        Returns:
            Decay coefficient (1/day).
        """
        ...

    def get_rain_conc(self, idx: Union[int, str]) -> float:
        """Return the rain concentration.

        Args:
            idx: Pollutant index (int) or ID (str).

        Returns:
            Rain concentration.
        """
        ...

    def get_gw_conc(self, idx: Union[int, str]) -> float:
        """Return the groundwater concentration.

        Args:
            idx: Pollutant index (int) or ID (str).

        Returns:
            Groundwater concentration.
        """
        ...

    def get_init_conc(self, idx: Union[int, str]) -> float:
        """Return the initial concentration.

        Args:
            idx: Pollutant index (int) or ID (str).

        Returns:
            Initial concentration.
        """
        ...

    def get_rdii_conc(self, idx: Union[int, str]) -> float:
        """Return the RDII concentration.

        Args:
            idx: Pollutant index (int) or ID (str).

        Returns:
            RDII concentration.
        """
        ...

    def get_mwt(self, idx: Union[int, str]) -> float:
        """Return the molecular weight.

        Args:
            idx: Pollutant index (int) or ID (str).

        Returns:
            Molecular weight.
        """
        ...

    def get_co_pollutant(self, idx: Union[int, str]) -> tuple[int, float]:
        """Return the co-pollutant index and fraction.

        Args:
            idx: Pollutant index (int) or ID (str).

        Returns:
            Tuple of (co-pollutant index, fraction).
        """
        ...

    def get_snow_only(self, idx: Union[int, str]) -> bool:
        """Return whether the pollutant applies only to snow events.

        Args:
            idx: Pollutant index (int) or ID (str).

        Returns:
            ``True`` if snow-only.
        """
        ...

    # ------------------------------------------------------------------
    # Property setters
    # ------------------------------------------------------------------

    def set_kdecay(self, idx: Union[int, str], k: float) -> None:
        """Set the first-order decay coefficient.

        Args:
            idx: Pollutant index (int) or ID (str).
            k: Decay coefficient (1/day).
        """
        ...

    def set_rain_conc(self, idx: Union[int, str], conc: float) -> None:
        """Set the rain concentration.

        Args:
            idx: Pollutant index (int) or ID (str).
            conc: Rain concentration.
        """
        ...

    def set_gw_conc(self, idx: Union[int, str], conc: float) -> None:
        """Set the groundwater concentration.

        Args:
            idx: Pollutant index (int) or ID (str).
            conc: Groundwater concentration.
        """
        ...

    def set_init_conc(self, idx: Union[int, str], conc: float) -> None:
        """Set the initial concentration.

        Args:
            idx: Pollutant index (int) or ID (str).
            conc: Initial concentration.
        """
        ...

    def set_rdii_conc(self, idx: Union[int, str], conc: float) -> None:
        """Set the RDII concentration.

        Args:
            idx: Pollutant index (int) or ID (str).
            conc: RDII concentration.
        """
        ...

    def set_mwt(self, idx: Union[int, str], mwt: float) -> None:
        """Set the molecular weight.

        Args:
            idx: Pollutant index (int) or ID (str).
            mwt: Molecular weight.
        """
        ...

    def set_co_pollutant(
        self, idx: Union[int, str], co_idx: int, frac: float
    ) -> None:
        """Set the co-pollutant relationship.

        Args:
            idx: Pollutant index (int) or ID (str).
            co_idx: Co-pollutant index.
            frac: Fraction of co-pollutant.
        """
        ...

    def set_snow_only(self, idx: Union[int, str], flag: bool) -> None:
        """Set whether the pollutant applies only to snow events.

        Args:
            idx: Pollutant index (int) or ID (str).
            flag: ``True`` for snow-only.
        """
        ...

    # ------------------------------------------------------------------
    # Runtime quality injection
    # ------------------------------------------------------------------

    def set_node_quality(
        self,
        node_idx: Union[int, str],
        pollut_idx: int,
        conc: float,
    ) -> None:
        """Inject a pollutant concentration at a node.

        Args:
            node_idx: Node index (int) or node ID (str).
            pollut_idx: Pollutant index.
            conc: Concentration to inject.
        """
        ...

    def set_link_quality(
        self,
        link_idx: Union[int, str],
        pollut_idx: int,
        conc: float,
    ) -> None:
        """Inject a pollutant concentration at a link.

        Args:
            link_idx: Link index (int) or link ID (str).
            pollut_idx: Pollutant index.
            conc: Concentration to inject.
        """
        ...
