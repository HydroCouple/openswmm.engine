"""
Pollutant Access
================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

The :class:`Pollutants` class provides access to pollutant properties and
runtime water-quality injection during a simulation.

.. code-block:: python

    from openswmm.engine import Solver, Pollutants

    with Solver("model.inp", "model.rpt", "model.out") as s:
        poll = Pollutants(s)
        print(f"Pollutant count: {poll.count()}")
        while s.state == EngineState.RUNNING:
            if s.step() != 0:
                break
            conc = poll.get_init_conc("TSS")         # by name
            poll.set_node_quality("J1", 0, 12.5)     # inject at node
"""

# cython: language_level=3

from ._common cimport *


class Pollutants:
    """Access pollutant properties and inject quality at nodes/links.

    All per-element methods accept either an integer index or a string
    pollutant ID.  When a string is passed it is resolved via
    L{get_index}.

    @ivar _solver: The owning solver instance whose engine handle is used
        for every C call.
    @type _solver: L{Solver}

    @param solver: An active L{Solver} instance. The solver must remain
        alive for the lifetime of this object.
    @type solver: L{Solver}
    """

    def __init__(self, solver):
        """Construct a L{Pollutants} accessor bound to C{solver}.

        @param solver: An active L{Solver} instance whose engine handle
            will be used for all subsequent pollutant operations.
        @type solver: L{Solver}
        """
        self._solver = solver

    def _resolve(self, idx) -> int:
        """Resolve C{idx} to an integer pollutant index.

        @param idx: Integer index or string pollutant ID.
        @type idx: Union[int, str]
        @return: Integer pollutant index.
        @rtype: int
        @raise KeyError: If a string ID is not found in the model.
        """
        cdef int i
        if isinstance(idx, str):
            i = self.get_index(idx)
            if i < 0:
                raise KeyError(f"Pollutant '{idx}' not found")
            return i
        return idx

    def _resolve_node(self, idx) -> int:
        """Resolve a node index (int) or node ID (str) to an integer index.

        @param idx: Node index or node ID.
        @type idx: Union[int, str]
        @return: Integer node index.
        @rtype: int
        @raise KeyError: If a string ID is not found in the model.
        """
        cdef SWMM_Engine h
        cdef bytes b
        cdef int i
        if isinstance(idx, str):
            h = <SWMM_Engine><size_t>self._solver.handle
            b = idx.encode('utf-8')
            i = swmm_node_index(h, b)
            if i < 0:
                raise KeyError(f"Node '{idx}' not found")
            return i
        return idx

    def _resolve_link(self, idx) -> int:
        """Resolve a link index (int) or link ID (str) to an integer index.

        @param idx: Link index or link ID.
        @type idx: Union[int, str]
        @return: Integer link index.
        @rtype: int
        @raise KeyError: If a string ID is not found in the model.
        """
        cdef SWMM_Engine h
        cdef bytes b
        cdef int i
        if isinstance(idx, str):
            h = <SWMM_Engine><size_t>self._solver.handle
            b = idx.encode('utf-8')
            i = swmm_link_index(h, b)
            if i < 0:
                raise KeyError(f"Link '{idx}' not found")
            return i
        return idx

    # ====================================================================
    # Identification & lookup
    # ====================================================================

    def count(self) -> int:
        """Return the number of pollutants in the model.

        @return: Pollutant count.
        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_pollutant_count(h)

    def get_index(self, str pollut_id) -> int:
        """Return the integer index of a pollutant by its string ID.

        @param pollut_id: Pollutant identifier string.
        @type pollut_id: str
        @return: Pollutant index, or C{-1} if not found.
        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = pollut_id.encode('utf-8')
        return swmm_pollutant_index(h, b)

    def get_id(self, int idx) -> str:
        """Return the string ID of a pollutant by index.

        @param idx: Pollutant index.
        @type idx: int
        @return: Pollutant ID string, or empty string if C{idx} is invalid.
        @rtype: str
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef const char* raw = swmm_pollutant_id(h, idx)
        return raw.decode('utf-8') if raw != NULL else ""

    # ====================================================================
    # Pollutant properties (getters)
    # ====================================================================

    def get_units(self, idx) -> int:
        """Return the concentration units code for a pollutant.

        @param idx: Pollutant index (int) or ID (str).
        @type idx: Union[int, str]
        @return: Units code -- see L{ConcentrationUnits}.
        @rtype: int
        @raise EngineError: If the underlying C{swmm_pollutant_get_units}
            call returns a non-zero error code.
        @raise KeyError: If C{idx} is a string not found in the model.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_pollutant_get_units(h, i, &v))
        return v

    def get_kdecay(self, idx) -> float:
        """Return the first-order decay coefficient.

        @param idx: Pollutant index (int) or ID (str).
        @type idx: Union[int, str]
        @return: Decay coefficient (1/day).
        @rtype: float
        @raise EngineError: If the underlying C{swmm_pollutant_get_kdecay}
            call returns a non-zero error code.
        @raise KeyError: If C{idx} is a string not found in the model.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_pollutant_get_kdecay(h, i, &v))
        return v

    def get_rain_conc(self, idx) -> float:
        """Return the rain concentration.

        @param idx: Pollutant index (int) or ID (str).
        @type idx: Union[int, str]
        @return: Rain concentration.
        @rtype: float
        @raise EngineError: If the underlying
            C{swmm_pollutant_get_rain_conc} call returns a non-zero error
            code.
        @raise KeyError: If C{idx} is a string not found in the model.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_pollutant_get_rain_conc(h, i, &v))
        return v

    def get_gw_conc(self, idx) -> float:
        """Return the groundwater concentration.

        @param idx: Pollutant index (int) or ID (str).
        @type idx: Union[int, str]
        @return: Groundwater concentration.
        @rtype: float
        @raise EngineError: If the underlying C{swmm_pollutant_get_gw_conc}
            call returns a non-zero error code.
        @raise KeyError: If C{idx} is a string not found in the model.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_pollutant_get_gw_conc(h, i, &v))
        return v

    def get_init_conc(self, idx) -> float:
        """Return the initial concentration.

        @param idx: Pollutant index (int) or ID (str).
        @type idx: Union[int, str]
        @return: Initial concentration.
        @rtype: float
        @raise EngineError: If the underlying
            C{swmm_pollutant_get_init_conc} call returns a non-zero error
            code.
        @raise KeyError: If C{idx} is a string not found in the model.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_pollutant_get_init_conc(h, i, &v))
        return v

    def get_rdii_conc(self, idx) -> float:
        """Return the RDII concentration.

        @param idx: Pollutant index (int) or ID (str).
        @type idx: Union[int, str]
        @return: RDII concentration.
        @rtype: float
        @raise EngineError: If the underlying
            C{swmm_pollutant_get_rdii_conc} call returns a non-zero error
            code.
        @raise KeyError: If C{idx} is a string not found in the model.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_pollutant_get_rdii_conc(h, i, &v))
        return v

    def get_mwt(self, idx) -> float:
        """Return the molecular weight.

        @param idx: Pollutant index (int) or ID (str).
        @type idx: Union[int, str]
        @return: Molecular weight.
        @rtype: float
        @raise EngineError: If the underlying C{swmm_pollutant_get_mwt}
            call returns a non-zero error code.
        @raise KeyError: If C{idx} is a string not found in the model.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_pollutant_get_mwt(h, i, &v))
        return v

    def get_co_pollutant(self, idx) -> tuple:
        """Return the co-pollutant index and fraction.

        @param idx: Pollutant index (int) or ID (str).
        @type idx: Union[int, str]
        @return: Tuple of C{(co_pollutant_index, fraction)}.
        @rtype: tuple
        @raise EngineError: If the underlying
            C{swmm_pollutant_get_co_pollutant} call returns a non-zero
            error code.
        @raise KeyError: If C{idx} is a string not found in the model.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int co_idx = 0
        cdef double frac = 0.0
        _check(swmm_pollutant_get_co_pollutant(h, i, &co_idx, &frac))
        return (co_idx, frac)

    def get_snow_only(self, idx) -> bool:
        """Return whether the pollutant applies only to snow events.

        @param idx: Pollutant index (int) or ID (str).
        @type idx: Union[int, str]
        @return: C{True} if the pollutant is snow-only.
        @rtype: bool
        @raise EngineError: If the underlying
            C{swmm_pollutant_get_snow_only} call returns a non-zero error
            code.
        @raise KeyError: If C{idx} is a string not found in the model.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int flag = 0
        _check(swmm_pollutant_get_snow_only(h, i, &flag))
        return bool(flag)

    # ====================================================================
    # Pollutant properties (setters)
    # ====================================================================

    def set_kdecay(self, idx, double k):
        """Set the first-order decay coefficient.

        @param idx: Pollutant index (int) or ID (str).
        @type idx: Union[int, str]
        @param k: Decay coefficient (1/day).
        @type k: float
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying C{swmm_pollutant_set_kdecay}
            call returns a non-zero error code.
        @raise KeyError: If C{idx} is a string not found in the model.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_pollutant_set_kdecay(h, i, k))

    def set_rain_conc(self, idx, double conc):
        """Set the rain concentration.

        @param idx: Pollutant index (int) or ID (str).
        @type idx: Union[int, str]
        @param conc: Rain concentration.
        @type conc: float
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying
            C{swmm_pollutant_set_rain_conc} call returns a non-zero error
            code.
        @raise KeyError: If C{idx} is a string not found in the model.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_pollutant_set_rain_conc(h, i, conc))

    def set_gw_conc(self, idx, double conc):
        """Set the groundwater concentration.

        @param idx: Pollutant index (int) or ID (str).
        @type idx: Union[int, str]
        @param conc: Groundwater concentration.
        @type conc: float
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying C{swmm_pollutant_set_gw_conc}
            call returns a non-zero error code.
        @raise KeyError: If C{idx} is a string not found in the model.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_pollutant_set_gw_conc(h, i, conc))

    def set_init_conc(self, idx, double conc):
        """Set the initial concentration.

        @param idx: Pollutant index (int) or ID (str).
        @type idx: Union[int, str]
        @param conc: Initial concentration.
        @type conc: float
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying
            C{swmm_pollutant_set_init_conc} call returns a non-zero error
            code.
        @raise KeyError: If C{idx} is a string not found in the model.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_pollutant_set_init_conc(h, i, conc))

    def set_rdii_conc(self, idx, double conc):
        """Set the RDII concentration.

        @param idx: Pollutant index (int) or ID (str).
        @type idx: Union[int, str]
        @param conc: RDII concentration.
        @type conc: float
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying
            C{swmm_pollutant_set_rdii_conc} call returns a non-zero error
            code.
        @raise KeyError: If C{idx} is a string not found in the model.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_pollutant_set_rdii_conc(h, i, conc))

    def set_mwt(self, idx, double mwt):
        """Set the molecular weight.

        @param idx: Pollutant index (int) or ID (str).
        @type idx: Union[int, str]
        @param mwt: Molecular weight.
        @type mwt: float
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying C{swmm_pollutant_set_mwt}
            call returns a non-zero error code.
        @raise KeyError: If C{idx} is a string not found in the model.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_pollutant_set_mwt(h, i, mwt))

    def set_co_pollutant(self, idx, int co_idx, double frac):
        """Set the co-pollutant relationship.

        @param idx: Pollutant index (int) or ID (str).
        @type idx: Union[int, str]
        @param co_idx: Co-pollutant index.
        @type co_idx: int
        @param frac: Fraction of co-pollutant contribution.
        @type frac: float
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying
            C{swmm_pollutant_set_co_pollutant} call returns a non-zero
            error code.
        @raise KeyError: If C{idx} is a string not found in the model.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_pollutant_set_co_pollutant(h, i, co_idx, frac))

    def set_snow_only(self, idx, bint flag):
        """Set whether the pollutant applies only to snow events.

        @param idx: Pollutant index (int) or ID (str).
        @type idx: Union[int, str]
        @param flag: C{True} for snow-only.
        @type flag: bool
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying
            C{swmm_pollutant_set_snow_only} call returns a non-zero error
            code.
        @raise KeyError: If C{idx} is a string not found in the model.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_pollutant_set_snow_only(h, i, flag))

    # ====================================================================
    # Per-element concentrations (runtime injection)
    # ====================================================================

    def set_node_quality(self, node_idx, int pollut_idx, double conc):
        """Inject a pollutant concentration at a node.

        @param node_idx: Node index (int) or node ID (str).
        @type node_idx: Union[int, str]
        @param pollut_idx: Pollutant index.
        @type pollut_idx: int
        @param conc: Concentration to inject.
        @type conc: float
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying C{swmm_node_set_quality}
            call returns a non-zero error code.
        @raise KeyError: If C{node_idx} is a string not found in the model.
        """
        cdef int ni = self._resolve_node(node_idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_quality(h, ni, pollut_idx, conc))

    def set_link_quality(self, link_idx, int pollut_idx, double conc):
        """Inject a pollutant concentration at a link.

        @param link_idx: Link index (int) or link ID (str).
        @type link_idx: Union[int, str]
        @param pollut_idx: Pollutant index.
        @type pollut_idx: int
        @param conc: Concentration to inject.
        @type conc: float
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying C{swmm_link_set_quality}
            call returns a non-zero error code.
        @raise KeyError: If C{link_idx} is a string not found in the model.
        """
        cdef int li = self._resolve_link(link_idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_quality(h, li, pollut_idx, conc))
