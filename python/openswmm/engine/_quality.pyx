"""
Water Quality Access
====================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

The :class:`Quality` class provides access to landuse, buildup, washoff,
and treatment parameters during a simulation.

.. code-block:: python

    from openswmm.engine import Solver
    from openswmm.engine._quality import Quality

    with Solver("model.inp", "model.rpt", "model.out") as s:
        q = Quality(s)
        print(f"Landuse count: {q.landuse_count()}")
"""

# cython: language_level=3

from ._common cimport *


class Quality:
    """Access landuse, buildup, washoff, and treatment parameters.

    @ivar _solver: The owning solver instance whose engine handle is used
        for every C call.
    @type _solver: L{Solver}

    @param solver: An active L{Solver} instance.
    @type solver: L{Solver}
    """

    def __init__(self, solver):
        """Construct a L{Quality} accessor bound to C{solver}.

        @param solver: An active L{Solver} instance whose engine handle
            will be used for all subsequent water-quality operations.
        @type solver: L{Solver}
        """
        self._solver = solver

    # ====================================================================
    # Landuse
    # ====================================================================

    def landuse_count(self) -> int:
        """Return the number of land uses.

        @return: Land use count.
        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_landuse_count(h)

    def landuse_index(self, str id) -> int:
        """Return the index of a land use by ID.

        @param id: Land use identifier.
        @type id: str
        @return: Index, or C{-1} if not found.
        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = id.encode('utf-8')
        return swmm_landuse_index(h, b)

    def landuse_id(self, int idx) -> str:
        """Return the ID string of a land use by index.

        @param idx: Land use index.
        @type idx: int
        @return: Land use identifier, or empty string if C{idx} is invalid.
        @rtype: str
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef const char* s = swmm_landuse_id(h, idx)
        if s == NULL:
            return ""
        return s.decode('utf-8')

    def landuse_set_sweep_interval(self, int idx, double days):
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
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_landuse_set_sweep_interval(h, idx, days))

    def landuse_get_sweep_interval(self, int idx) -> float:
        """Return the street-sweeping interval for a land use.

        @param idx: Land use index.
        @type idx: int
        @return: Sweep interval in days.
        @rtype: float
        @raise EngineError: If the underlying
            C{swmm_landuse_get_sweep_interval} call returns a non-zero
            error code.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_landuse_get_sweep_interval(h, idx, &v))
        return v

    def landuse_set_sweep_removal(self, int idx, double frac):
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
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_landuse_set_sweep_removal(h, idx, frac))

    def landuse_get_sweep_removal(self, int idx) -> float:
        """Return the street-sweeping removal fraction for a land use.

        @param idx: Land use index.
        @type idx: int
        @return: Removal fraction in C{[0, 1]}.
        @rtype: float
        @raise EngineError: If the underlying
            C{swmm_landuse_get_sweep_removal} call returns a non-zero
            error code.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_landuse_get_sweep_removal(h, idx, &v))
        return v

    # ====================================================================
    # Buildup functions
    # ====================================================================

    def buildup_set(self, int lu_idx, int pollut_idx, int func_type,
                    double c1, double c2, double c3, int normalizer):
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
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_buildup_set(h, lu_idx, pollut_idx, func_type,
                                 c1, c2, c3, normalizer))

    def buildup_get(self, int lu_idx, int pollut_idx) -> dict:
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
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int func_type = 0
        cdef double c1 = 0.0
        cdef double c2 = 0.0
        cdef double c3 = 0.0
        cdef int normalizer = 0
        _check(swmm_buildup_get(h, lu_idx, pollut_idx,
                                 &func_type, &c1, &c2, &c3, &normalizer))
        return {
            'func_type': func_type,
            'c1': c1,
            'c2': c2,
            'c3': c3,
            'normalizer': normalizer,
        }

    # ====================================================================
    # Washoff functions
    # ====================================================================

    def washoff_set(self, int lu_idx, int pollut_idx, int func_type,
                    double coeff, double expon, double sweep_effic,
                    double bmp_effic):
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
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_washoff_set(h, lu_idx, pollut_idx, func_type,
                                 coeff, expon, sweep_effic, bmp_effic))

    def washoff_get(self, int lu_idx, int pollut_idx) -> dict:
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
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int func_type = 0
        cdef double coeff = 0.0
        cdef double expon = 0.0
        cdef double sweep_effic = 0.0
        cdef double bmp_effic = 0.0
        _check(swmm_washoff_get(h, lu_idx, pollut_idx,
                                 &func_type, &coeff, &expon,
                                 &sweep_effic, &bmp_effic))
        return {
            'func_type': func_type,
            'coeff': coeff,
            'expon': expon,
            'sweep_effic': sweep_effic,
            'bmp_effic': bmp_effic,
        }

    # ====================================================================
    # Treatment
    # ====================================================================

    def treatment_set(self, int node_idx, int pollut_idx, str expression):
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
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = expression.encode('utf-8')
        _check(swmm_treatment_set(h, node_idx, pollut_idx, b))

    def treatment_get(self, int node_idx, int pollut_idx) -> str:
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
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef char buf[4096]
        _check(swmm_treatment_get(h, node_idx, pollut_idx, buf, 4096))
        return buf.decode('utf-8')

    def treatment_clear(self, int node_idx, int pollut_idx):
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
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_treatment_clear(h, node_idx, pollut_idx))
