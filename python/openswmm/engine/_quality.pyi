"""
Water Quality Access
====================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

Type stubs for :mod:`openswmm.engine._quality`.

The :class:`Quality` class provides access to landuse, buildup, washoff,
and treatment parameters during a simulation.
"""

from ._solver import Solver


class Quality:
    """Access landuse, buildup, washoff, and treatment parameters.

    @ivar _solver: The owning solver instance whose engine handle is used
        for every C call.
    @type _solver: L{Solver}

    Example::

        from openswmm.engine import Solver
        from openswmm.engine._quality import Quality

        with Solver("model.inp", "model.rpt", "model.out") as s:
            q = Quality(s)
            print(f"Landuse count: {q.landuse_count()}")

    @param solver: An active L{Solver} instance. The solver must remain
        alive for the lifetime of this object.
    @type solver: L{Solver}
    """

    def __init__(self, solver: Solver) -> None:
        """Construct a L{Quality} accessor bound to C{solver}.

        @param solver: An active L{Solver} instance whose engine handle
            will be used for all subsequent water-quality operations.
        @type solver: L{Solver}
        """
        ...

    # ====================================================================
    # Landuse
    # ====================================================================

    def landuse_count(self) -> int:
        """Return the number of land uses in the model.

        @return: Land use count.
        @rtype: int
        """
        ...

    def landuse_index(self, id: str) -> int:
        """Return the index of a land use by ID.

        @param id: Land use identifier.
        @type id: str
        @return: Index, or C{-1} if not found.
        @rtype: int
        """
        ...

    def landuse_id(self, idx: int) -> str:
        """Return the ID string of a land use by index.

        @param idx: Land use index.
        @type idx: int
        @return: Land use identifier, or empty string if C{idx} is invalid.
        @rtype: str
        """
        ...

    def landuse_set_sweep_interval(self, idx: int, days: float) -> None:
        """Set the street-sweeping interval for a land use.

        @param idx: Land use index.
        @type idx: int
        @param days: Sweep interval in days.
        @type days: float
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying
            C{swmm_landuse_set_sweep_interval} call returns a non-zero
            error code.
        """
        ...

    def landuse_get_sweep_interval(self, idx: int) -> float:
        """Return the street-sweeping interval for a land use.

        @param idx: Land use index.
        @type idx: int
        @return: Sweep interval in days.
        @rtype: float
        @raise EngineError: If the underlying
            C{swmm_landuse_get_sweep_interval} call returns a non-zero
            error code.
        """
        ...

    def landuse_set_sweep_removal(self, idx: int, frac: float) -> None:
        """Set the street-sweeping removal fraction for a land use.

        @param idx: Land use index.
        @type idx: int
        @param frac: Removal fraction in C{[0, 1]}.
        @type frac: float
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying
            C{swmm_landuse_set_sweep_removal} call returns a non-zero
            error code.
        """
        ...

    def landuse_get_sweep_removal(self, idx: int) -> float:
        """Return the street-sweeping removal fraction for a land use.

        @param idx: Land use index.
        @type idx: int
        @return: Removal fraction in C{[0, 1]}.
        @rtype: float
        @raise EngineError: If the underlying
            C{swmm_landuse_get_sweep_removal} call returns a non-zero
            error code.
        """
        ...

    # ====================================================================
    # Buildup functions
    # ====================================================================

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

        @param lu_idx: Land use index.
        @type lu_idx: int
        @param pollut_idx: Pollutant index.
        @type pollut_idx: int
        @param func_type: Buildup function type code -- see L{BuildupFunc}.
        @type func_type: int
        @param c1: Coefficient C1.
        @type c1: float
        @param c2: Coefficient C2.
        @type c2: float
        @param c3: Coefficient C3.
        @type c3: float
        @param normalizer: Normalizer code (e.g. by area or curb-length).
        @type normalizer: int
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying C{swmm_buildup_set} call
            returns a non-zero error code.
        """
        ...

    def buildup_get(self, lu_idx: int, pollut_idx: int) -> dict:
        """Return buildup parameters for a land use / pollutant pair.

        @param lu_idx: Land use index.
        @type lu_idx: int
        @param pollut_idx: Pollutant index.
        @type pollut_idx: int
        @return: Dict with keys C{func_type}, C{c1}, C{c2}, C{c3},
            C{normalizer}.
        @rtype: dict
        @raise EngineError: If the underlying C{swmm_buildup_get} call
            returns a non-zero error code.
        """
        ...

    # ====================================================================
    # Washoff functions
    # ====================================================================

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

        @param lu_idx: Land use index.
        @type lu_idx: int
        @param pollut_idx: Pollutant index.
        @type pollut_idx: int
        @param func_type: Washoff function type code -- see L{WashoffFunc}.
        @type func_type: int
        @param coeff: Washoff coefficient.
        @type coeff: float
        @param expon: Washoff exponent.
        @type expon: float
        @param sweep_effic: Street-sweeping efficiency in C{[0, 1]}.
        @type sweep_effic: float
        @param bmp_effic: BMP removal efficiency in C{[0, 1]}.
        @type bmp_effic: float
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying C{swmm_washoff_set} call
            returns a non-zero error code.
        """
        ...

    def washoff_get(self, lu_idx: int, pollut_idx: int) -> dict:
        """Return washoff parameters for a land use / pollutant pair.

        @param lu_idx: Land use index.
        @type lu_idx: int
        @param pollut_idx: Pollutant index.
        @type pollut_idx: int
        @return: Dict with keys C{func_type}, C{coeff}, C{expon},
            C{sweep_effic}, C{bmp_effic}.
        @rtype: dict
        @raise EngineError: If the underlying C{swmm_washoff_get} call
            returns a non-zero error code.
        """
        ...

    # ====================================================================
    # Treatment
    # ====================================================================

    def treatment_set(
        self, node_idx: int, pollut_idx: int, expression: str
    ) -> None:
        """Set a treatment expression for a node / pollutant pair.

        @param node_idx: Node index.
        @type node_idx: int
        @param pollut_idx: Pollutant index.
        @type pollut_idx: int
        @param expression: Treatment expression string (SWMM treatment syntax).
        @type expression: str
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying C{swmm_treatment_set} call
            returns a non-zero error code (e.g. invalid expression).
        """
        ...

    def treatment_get(self, node_idx: int, pollut_idx: int) -> str:
        """Return the treatment expression for a node / pollutant pair.

        @param node_idx: Node index.
        @type node_idx: int
        @param pollut_idx: Pollutant index.
        @type pollut_idx: int
        @return: Treatment expression string.
        @rtype: str
        @raise EngineError: If the underlying C{swmm_treatment_get} call
            returns a non-zero error code.
        """
        ...

    def treatment_clear(self, node_idx: int, pollut_idx: int) -> None:
        """Remove the treatment expression for a node / pollutant pair.

        @param node_idx: Node index.
        @type node_idx: int
        @param pollut_idx: Pollutant index.
        @type pollut_idx: int
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying C{swmm_treatment_clear} call
            returns a non-zero error code.
        """
        ...
