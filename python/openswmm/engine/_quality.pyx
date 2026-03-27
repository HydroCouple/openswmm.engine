"""
Water Quality Access
====================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
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

    :param solver: An active :class:`~openswmm.engine.Solver` instance.
    """

    def __init__(self, solver):
        self._solver = solver

    # ------------------------------------------------------------------
    # Landuse
    # ------------------------------------------------------------------

    def landuse_count(self) -> int:
        """Return the number of land uses."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_landuse_count(h)

    def landuse_index(self, str id) -> int:
        """Return the index of a land use by ID.

        :param id: Land use identifier.
        :returns: Index, or -1 if not found.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = id.encode('utf-8')
        return swmm_landuse_index(h, b)

    def landuse_id(self, int idx) -> str:
        """Return the ID string of a land use by index.

        :param idx: Land use index.
        :returns: Land use identifier.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef const char* s = swmm_landuse_id(h, idx)
        if s == NULL:
            return ""
        return s.decode('utf-8')

    def landuse_set_sweep_interval(self, int idx, double days):
        """Set the street-sweeping interval for a land use.

        :param idx: Land use index.
        :param days: Sweep interval in days.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_landuse_set_sweep_interval(h, idx, days))

    def landuse_get_sweep_interval(self, int idx) -> float:
        """Return the street-sweeping interval for a land use.

        :param idx: Land use index.
        :returns: Sweep interval in days.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_landuse_get_sweep_interval(h, idx, &v))
        return v

    def landuse_set_sweep_removal(self, int idx, double frac):
        """Set the street-sweeping removal fraction for a land use.

        :param idx: Land use index.
        :param frac: Removal fraction (0-1).
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_landuse_set_sweep_removal(h, idx, frac))

    def landuse_get_sweep_removal(self, int idx) -> float:
        """Return the street-sweeping removal fraction for a land use.

        :param idx: Land use index.
        :returns: Removal fraction (0-1).
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_landuse_get_sweep_removal(h, idx, &v))
        return v

    # ------------------------------------------------------------------
    # Buildup
    # ------------------------------------------------------------------

    def buildup_set(self, int lu_idx, int pollut_idx, int func_type,
                    double c1, double c2, double c3, int normalizer):
        """Set buildup parameters for a land use / pollutant pair.

        :param lu_idx: Land use index.
        :param pollut_idx: Pollutant index.
        :param func_type: Buildup function type code.
        :param c1: Coefficient C1.
        :param c2: Coefficient C2.
        :param c3: Coefficient C3.
        :param normalizer: Normalizer code.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_buildup_set(h, lu_idx, pollut_idx, func_type,
                                 c1, c2, c3, normalizer))

    def buildup_get(self, int lu_idx, int pollut_idx) -> dict:
        """Return buildup parameters for a land use / pollutant pair.

        :param lu_idx: Land use index.
        :param pollut_idx: Pollutant index.
        :returns: Dict with keys: func_type, c1, c2, c3, normalizer.
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

    # ------------------------------------------------------------------
    # Washoff
    # ------------------------------------------------------------------

    def washoff_set(self, int lu_idx, int pollut_idx, int func_type,
                    double coeff, double expon, double sweep_effic,
                    double bmp_effic):
        """Set washoff parameters for a land use / pollutant pair.

        :param lu_idx: Land use index.
        :param pollut_idx: Pollutant index.
        :param func_type: Washoff function type code.
        :param coeff: Washoff coefficient.
        :param expon: Washoff exponent.
        :param sweep_effic: Street-sweeping efficiency (0-1).
        :param bmp_effic: BMP removal efficiency (0-1).
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_washoff_set(h, lu_idx, pollut_idx, func_type,
                                 coeff, expon, sweep_effic, bmp_effic))

    def washoff_get(self, int lu_idx, int pollut_idx) -> dict:
        """Return washoff parameters for a land use / pollutant pair.

        :param lu_idx: Land use index.
        :param pollut_idx: Pollutant index.
        :returns: Dict with keys: func_type, coeff, expon, sweep_effic, bmp_effic.
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

    # ------------------------------------------------------------------
    # Treatment
    # ------------------------------------------------------------------

    def treatment_set(self, int node_idx, int pollut_idx, str expression):
        """Set a treatment expression for a node / pollutant pair.

        :param node_idx: Node index.
        :param pollut_idx: Pollutant index.
        :param expression: Treatment expression string.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = expression.encode('utf-8')
        _check(swmm_treatment_set(h, node_idx, pollut_idx, b))

    def treatment_get(self, int node_idx, int pollut_idx) -> str:
        """Return the treatment expression for a node / pollutant pair.

        :param node_idx: Node index.
        :param pollut_idx: Pollutant index.
        :returns: Treatment expression string.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef char buf[4096]
        _check(swmm_treatment_get(h, node_idx, pollut_idx, buf, 4096))
        return buf.decode('utf-8')

    def treatment_clear(self, int node_idx, int pollut_idx):
        """Remove the treatment expression for a node / pollutant pair.

        :param node_idx: Node index.
        :param pollut_idx: Pollutant index.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_treatment_clear(h, node_idx, pollut_idx))
