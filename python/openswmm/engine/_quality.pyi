"""
Water Quality Access
====================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

Type stubs for :mod:`openswmm.engine._quality`.

The :class:`Quality` class provides access to landuse, buildup, washoff,
and treatment parameters during a simulation.
"""

from ._solver import Solver


class Quality:
    """Access landuse, buildup, washoff, and treatment parameters.

    Args:
        solver: An active :class:`Solver` instance. The solver must remain
                alive for the lifetime of this object.

    Example::

        from openswmm.engine import Solver
        from openswmm.engine._quality import Quality

        with Solver("model.inp", "model.rpt", "model.out") as s:
            q = Quality(s)
            print(f"Landuse count: {q.landuse_count()}")
    """

    def __init__(self, solver: Solver) -> None: ...

    # ------------------------------------------------------------------
    # Landuse
    # ------------------------------------------------------------------

    def landuse_count(self) -> int:
        """Return the number of land uses in the model.

        Returns:
            Land use count.
        """
        ...

    def landuse_index(self, id: str) -> int:
        """Return the index of a land use by ID.

        Args:
            id: Land use identifier.

        Returns:
            Index, or -1 if not found.
        """
        ...

    def landuse_id(self, idx: int) -> str:
        """Return the ID string of a land use by index.

        Args:
            idx: Land use index.

        Returns:
            Land use identifier.
        """
        ...

    def landuse_set_sweep_interval(self, idx: int, days: float) -> None:
        """Set the street-sweeping interval for a land use.

        Args:
            idx: Land use index.
            days: Sweep interval in days.
        """
        ...

    def landuse_get_sweep_interval(self, idx: int) -> float:
        """Return the street-sweeping interval for a land use.

        Args:
            idx: Land use index.

        Returns:
            Sweep interval in days.
        """
        ...

    def landuse_set_sweep_removal(self, idx: int, frac: float) -> None:
        """Set the street-sweeping removal fraction for a land use.

        Args:
            idx: Land use index.
            frac: Removal fraction (0–1).
        """
        ...

    def landuse_get_sweep_removal(self, idx: int) -> float:
        """Return the street-sweeping removal fraction for a land use.

        Args:
            idx: Land use index.

        Returns:
            Removal fraction (0–1).
        """
        ...

    # ------------------------------------------------------------------
    # Buildup
    # ------------------------------------------------------------------

    def buildup_set(
        self,
        lu_idx: int,
        pollut_idx: int,
        func_type: int,
        c1: float,
        c2: float,
        c3: float,
        normalizer: int,
    ) -> None:
        """Set buildup parameters for a land use / pollutant pair.

        Args:
            lu_idx: Land use index.
            pollut_idx: Pollutant index.
            func_type: Buildup function type code (see
                :class:`~openswmm.engine.BuildupFunc`).
            c1: Coefficient C1.
            c2: Coefficient C2.
            c3: Coefficient C3.
            normalizer: Normalizer code.
        """
        ...

    def buildup_get(self, lu_idx: int, pollut_idx: int) -> dict:
        """Return buildup parameters for a land use / pollutant pair.

        Args:
            lu_idx: Land use index.
            pollut_idx: Pollutant index.

        Returns:
            Dict with keys: ``func_type``, ``c1``, ``c2``, ``c3``,
            ``normalizer``.
        """
        ...

    # ------------------------------------------------------------------
    # Washoff
    # ------------------------------------------------------------------

    def washoff_set(
        self,
        lu_idx: int,
        pollut_idx: int,
        func_type: int,
        coeff: float,
        expon: float,
        sweep_effic: float,
        bmp_effic: float,
    ) -> None:
        """Set washoff parameters for a land use / pollutant pair.

        Args:
            lu_idx: Land use index.
            pollut_idx: Pollutant index.
            func_type: Washoff function type code (see
                :class:`~openswmm.engine.WashoffFunc`).
            coeff: Washoff coefficient.
            expon: Washoff exponent.
            sweep_effic: Street-sweeping efficiency (0–1).
            bmp_effic: BMP removal efficiency (0–1).
        """
        ...

    def washoff_get(self, lu_idx: int, pollut_idx: int) -> dict:
        """Return washoff parameters for a land use / pollutant pair.

        Args:
            lu_idx: Land use index.
            pollut_idx: Pollutant index.

        Returns:
            Dict with keys: ``func_type``, ``coeff``, ``expon``,
            ``sweep_effic``, ``bmp_effic``.
        """
        ...

    # ------------------------------------------------------------------
    # Treatment
    # ------------------------------------------------------------------

    def treatment_set(
        self, node_idx: int, pollut_idx: int, expression: str
    ) -> None:
        """Set a treatment expression for a node / pollutant pair.

        Args:
            node_idx: Node index.
            pollut_idx: Pollutant index.
            expression: Treatment expression string.
        """
        ...

    def treatment_get(self, node_idx: int, pollut_idx: int) -> str:
        """Return the treatment expression for a node / pollutant pair.

        Args:
            node_idx: Node index.
            pollut_idx: Pollutant index.

        Returns:
            Treatment expression string.
        """
        ...

    def treatment_clear(self, node_idx: int, pollut_idx: int) -> None:
        """Remove the treatment expression for a node / pollutant pair.

        Args:
            node_idx: Node index.
            pollut_idx: Pollutant index.
        """
        ...
