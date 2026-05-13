"""
Pollutant Access
================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
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
    L{get_index}.

    @ivar _solver: The owning solver instance whose engine handle is used
        for every C call.
    @type _solver: L{Solver}

    Example::

        from openswmm.engine import Solver, Pollutants

        with Solver("model.inp", "model.rpt", "model.out") as s:
            poll = Pollutants(s)
            print(f"Pollutant count: {poll.count()}")
            while s.state == EngineState.RUNNING:
                if s.step() != 0:
                    break
                conc = poll.get_init_conc("TSS")
                poll.set_node_quality("J1", 0, 12.5)

    @param solver: An active L{Solver} instance. The solver must remain
        alive for the lifetime of this object.
    @type solver: L{Solver}
    """

    def __init__(self, solver: Solver) -> None:
        """Construct a L{Pollutants} accessor bound to C{solver}.

        @param solver: An active L{Solver} instance whose engine handle
            will be used for all subsequent pollutant operations.
        @type solver: L{Solver}
        """
        ...

    def _resolve(self, idx: Union[int, str]) -> int:
        """Resolve C{idx} to an integer pollutant index.

        @param idx: Integer index or string pollutant ID.
        @type idx: Union[int, str]
        @return: Integer pollutant index.
        @rtype: int
        @raise KeyError: If a string ID is not found in the model.
        """
        ...

    # ====================================================================
    # Identification & lookup
    # ====================================================================

    def count(self) -> int:
        """Return the number of pollutants in the model.

        @return: Pollutant count.
        @rtype: int
        """
        ...

    def get_index(self, pollut_id: str) -> int:
        """Return the integer index of a pollutant by its string ID.

        @param pollut_id: Pollutant identifier string.
        @type pollut_id: str
        @return: Pollutant index, or C{-1} if not found.
        @rtype: int
        """
        ...

    def get_id(self, idx: int) -> str:
        """Return the string ID of a pollutant by index.

        @param idx: Pollutant index.
        @type idx: int
        @return: Pollutant ID string, or empty string if C{idx} is invalid.
        @rtype: str
        """
        ...

    # ====================================================================
    # Pollutant properties (getters)
    # ====================================================================

    def get_units(self, idx: Union[int, str]) -> int:
        """Return the concentration units code for a pollutant.

        @param idx: Pollutant index (int) or ID (str).
        @type idx: Union[int, str]
        @return: Units code -- see L{ConcentrationUnits}.
        @rtype: int
        @raise EngineError: If the underlying C{swmm_pollutant_get_units}
            call returns a non-zero error code.
        @raise KeyError: If C{idx} is a string not found in the model.
        """
        ...

    def get_kdecay(self, idx: Union[int, str]) -> float:
        """Return the first-order decay coefficient.

        @param idx: Pollutant index (int) or ID (str).
        @type idx: Union[int, str]
        @return: Decay coefficient (1/day).
        @rtype: float
        @raise EngineError: If the underlying C{swmm_pollutant_get_kdecay}
            call returns a non-zero error code.
        @raise KeyError: If C{idx} is a string not found in the model.
        """
        ...

    def get_rain_conc(self, idx: Union[int, str]) -> float:
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
        ...

    def get_gw_conc(self, idx: Union[int, str]) -> float:
        """Return the groundwater concentration.

        @param idx: Pollutant index (int) or ID (str).
        @type idx: Union[int, str]
        @return: Groundwater concentration.
        @rtype: float
        @raise EngineError: If the underlying C{swmm_pollutant_get_gw_conc}
            call returns a non-zero error code.
        @raise KeyError: If C{idx} is a string not found in the model.
        """
        ...

    def get_init_conc(self, idx: Union[int, str]) -> float:
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
        ...

    def get_rdii_conc(self, idx: Union[int, str]) -> float:
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
        ...

    def get_mwt(self, idx: Union[int, str]) -> float:
        """Return the molecular weight.

        @param idx: Pollutant index (int) or ID (str).
        @type idx: Union[int, str]
        @return: Molecular weight.
        @rtype: float
        @raise EngineError: If the underlying C{swmm_pollutant_get_mwt}
            call returns a non-zero error code.
        @raise KeyError: If C{idx} is a string not found in the model.
        """
        ...

    def get_co_pollutant(self, idx: Union[int, str]) -> tuple[int, float]:
        """Return the co-pollutant index and fraction.

        @param idx: Pollutant index (int) or ID (str).
        @type idx: Union[int, str]
        @return: Tuple of C{(co_pollutant_index, fraction)}.
        @rtype: tuple[int, float]
        @raise EngineError: If the underlying
            C{swmm_pollutant_get_co_pollutant} call returns a non-zero
            error code.
        @raise KeyError: If C{idx} is a string not found in the model.
        """
        ...

    def get_snow_only(self, idx: Union[int, str]) -> bool:
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
        ...

    # ====================================================================
    # Pollutant properties (setters)
    # ====================================================================

    def set_kdecay(self, idx: Union[int, str], k: float) -> None:
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
        ...

    def set_rain_conc(self, idx: Union[int, str], conc: float) -> None:
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
        ...

    def set_gw_conc(self, idx: Union[int, str], conc: float) -> None:
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
        ...

    def set_init_conc(self, idx: Union[int, str], conc: float) -> None:
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
        ...

    def set_rdii_conc(self, idx: Union[int, str], conc: float) -> None:
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
        ...

    def set_mwt(self, idx: Union[int, str], mwt: float) -> None:
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
        ...

    def set_co_pollutant(
        self, idx: Union[int, str], co_idx: int, frac: float
    ) -> None:
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
        ...

    def set_snow_only(self, idx: Union[int, str], flag: bool) -> None:
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
        ...

    # ====================================================================
    # Per-element concentrations (runtime injection)
    # ====================================================================

    def set_node_quality(
        self,
        node_idx: Union[int, str],
        pollut_idx: int,
        conc: float,
    ) -> None:
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
        ...

    def set_link_quality(
        self,
        link_idx: Union[int, str],
        pollut_idx: int,
        conc: float,
    ) -> None:
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
        ...
