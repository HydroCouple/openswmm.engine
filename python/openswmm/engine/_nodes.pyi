"""
Node Access
===========

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

Type stubs for :mod:`openswmm.engine._nodes`.

The :class:`Nodes` class provides access to node properties during a
simulation.
"""

from typing import Tuple, Union

import numpy as np
import numpy.typing as npt

from ._solver import Solver


class Nodes:
    """Access node properties during a simulation.

    All per-element methods accept either an integer index or a string node
    ID.  When a string is passed it is resolved via L{get_index}.

    @param solver: An active L{Solver} instance. The solver must remain
        alive for the lifetime of this object.
    @type solver: L{Solver}

    Example::

        from openswmm.engine import Solver, Nodes

        with Solver("model.inp", "model.rpt", "model.out") as s:
            nodes = Nodes(s)
            depth = nodes.get_depth(0)      # by index
            depth = nodes.get_depth("J1")   # by name
    """

    def __init__(self, solver: Solver) -> None: ...

    def _resolve(self, idx: Union[int, str]) -> int:
        """Resolve C{idx} to an integer index.

        @param idx: Integer index or string node ID.
        @type idx: Union[int, str]
        @return: Integer index.
        @rtype: int
        @raise KeyError: If a string ID is not found.
        """
        ...

    # ====================================================================
    # Identification & lookup
    # ====================================================================

    def count(self) -> int:
        """Return the number of nodes in the model.

        @return: Node count.
        @rtype: int
        """
        ...

    def get_index(self, node_id: str) -> int:
        """Return the integer index of a node by its string ID.

        @param node_id: Node identifier string.
        @type node_id: str
        @return: Node index, or C{-1} if not found.
        @rtype: int
        """
        ...

    def get_id(self, idx: int) -> str:
        """Return the string ID of a node by index.

        @param idx: Node index.
        @type idx: int
        @return: Node ID string.
        @rtype: str
        """
        ...

    # ====================================================================
    # Construction (add/pop_last)
    # ====================================================================

    def add(self, node_id: str, node_type: int) -> int:
        """Add a node to the model (OPENED-state editing).

        Wraps C{swmm_node_add}. Valid in C{BUILDING} or C{OPENED} state.

        @param node_id: Unique node identifier.
        @type node_id: str
        @param node_type: Node type code (see L{NodeType}).
        @type node_type: int
        @return: Error code (C{0} on success).
        @rtype: int
        """
        ...

    def pop_last(self, node_id: str) -> int:
        """Remove the most recently added node (undo-of-add).

        Wraps C{swmm_node_pop_last}. Valid in C{BUILDING} or C{OPENED}
        state. C{node_id} must match the current tail; otherwise
        C{SWMM_ERR_BADINDEX} is returned.

        @param node_id: Expected tail node identifier.
        @type node_id: str
        @return: Error code (C{0} on success).
        @rtype: int
        """
        ...

    # ====================================================================
    # Geometry getters
    # ====================================================================

    def get_type(self, idx: Union[int, str]) -> int:
        """Return the node type code.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Node type code.
        @rtype: int
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def get_invert_elev(self, idx: Union[int, str]) -> float:
        """Return the invert elevation of a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Invert elevation (project length units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def get_max_depth(self, idx: Union[int, str]) -> float:
        """Return the maximum depth of a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Maximum depth (project length units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def get_surcharge_depth(self, idx: Union[int, str]) -> float:
        """Return the surcharge depth of a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Surcharge depth (project length units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def get_ponded_area(self, idx: Union[int, str]) -> float:
        """Return the ponded area of a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Ponded area (project area units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def get_initial_depth(self, idx: Union[int, str]) -> float:
        """Return the initial depth of a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Initial depth (project length units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def get_crown_elev(self, idx: Union[int, str]) -> float:
        """Return the crown elevation of a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Crown elevation (project length units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def get_full_volume(self, idx: Union[int, str]) -> float:
        """Return the full volume of a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Full volume (project volume units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def get_losses(self, idx: Union[int, str]) -> float:
        """Return the losses at a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Losses (project flow units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def get_outflow(self, idx: Union[int, str]) -> float:
        """Return the outflow from a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Outflow (project flow units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def get_degree(self, idx: Union[int, str]) -> int:
        """Return the number of links connected to a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Node degree (number of connected links).
        @rtype: int
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    # ====================================================================
    # Per-element hydraulic state (depth/head/volume/inflow)
    # ====================================================================

    def get_depth(self, idx: Union[int, str]) -> float:
        """Return the current water depth at a node.

        @param idx: Node index (int) or node ID (str). Strings are
            resolved via L{get_index}.
        @type idx: Union[int, str]
        @return: Water depth in project length units (feet for US, metres for SI).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def get_head(self, idx: Union[int, str]) -> float:
        """Return the hydraulic head at a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Hydraulic head (project length units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def get_volume(self, idx: Union[int, str]) -> float:
        """Return the volume stored at a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Volume (project volume units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def get_lateral_inflow(self, idx: Union[int, str]) -> float:
        """Return the lateral inflow at a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Lateral inflow (project flow units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def get_overflow(self, idx: Union[int, str]) -> float:
        """Return the overflow rate at a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Overflow rate (project flow units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def get_inflow(self, idx: Union[int, str]) -> float:
        """Return the total inflow at a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Total inflow (project flow units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    # ====================================================================
    # Per-element setters (geometry & runtime forcing)
    # ====================================================================

    def set_depth(self, idx: Union[int, str], value: float) -> None:
        """Set the water depth at a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param value: New depth (project length units).
        @type value: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def set_lateral_inflow(self, idx: Union[int, str], flow: float) -> None:
        """Prescribe a lateral inflow at a node (RUNNING state only).

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param flow: Lateral inflow rate (project flow units).
        @type flow: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def set_head_boundary(self, idx: Union[int, str], head: float) -> None:
        """Set a fixed head boundary condition at a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param head: Boundary head value (project length units).
        @type head: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def set_invert_elev(self, idx: Union[int, str], elev: float) -> None:
        """Set the invert elevation of a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param elev: Invert elevation (project length units).
        @type elev: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def set_max_depth(self, idx: Union[int, str], depth: float) -> None:
        """Set the maximum depth of a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param depth: Maximum depth (project length units).
        @type depth: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def set_surcharge_depth(self, idx: Union[int, str], depth: float) -> None:
        """Set the surcharge depth of a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param depth: Surcharge depth (project length units).
        @type depth: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def set_pond_area(self, idx: Union[int, str], area: float) -> None:
        """Set the ponded area of a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param area: Ponded area (project area units).
        @type area: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def set_initial_depth(self, idx: Union[int, str], depth: float) -> None:
        """Set the initial depth of a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param depth: Initial depth (project length units).
        @type depth: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    # ====================================================================
    # Pollutant/quality
    # ====================================================================

    def get_quality(self, idx: Union[int, str], pollutant_idx: int) -> float:
        """Return the concentration of a pollutant at a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param pollutant_idx: Pollutant index.
        @type pollutant_idx: int
        @return: Pollutant concentration.
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def set_quality_mass_flux(
        self, idx: Union[int, str], pollutant_idx: int, mass_rate: float
    ) -> None:
        """Inject a pollutant mass flux at a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param pollutant_idx: Pollutant index.
        @type pollutant_idx: int
        @param mass_rate: Mass injection rate (mass/time in model units).
        @type mass_rate: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    # ====================================================================
    # Storage node methods
    # ====================================================================

    def set_storage_curve(self, idx: Union[int, str], curve_idx: int) -> None:
        """Assign a storage curve to a storage node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param curve_idx: Curve index.
        @type curve_idx: int
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def get_storage_curve(self, idx: Union[int, str]) -> int:
        """Return the storage curve index for a storage node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Curve index.
        @rtype: int
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def set_storage_functional(
        self, idx: Union[int, str], a: float, b: float, c: float
    ) -> None:
        """Set the functional storage parameters (A, B, C) for a storage node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param a: Coefficient A.
        @type a: float
        @param b: Coefficient B.
        @type b: float
        @param c: Coefficient C.
        @type c: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def get_storage_functional(
        self, idx: Union[int, str]
    ) -> Tuple[float, float, float]:
        """Return the functional storage parameters (A, B, C) for a storage node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Tuple of C{(a, b, c)} coefficients.
        @rtype: Tuple[float, float, float]
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def set_storage_seep_rate(self, idx: Union[int, str], rate: float) -> None:
        """Set the seepage rate for a storage node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param rate: Seepage rate.
        @type rate: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def get_storage_seep_rate(self, idx: Union[int, str]) -> float:
        """Return the seepage rate for a storage node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Seepage rate.
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def set_exfil_params(
        self, idx: Union[int, str], suction: float, ksat: float, imd: float
    ) -> None:
        """Set exfiltration parameters for a storage node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param suction: Suction head.
        @type suction: float
        @param ksat: Saturated hydraulic conductivity.
        @type ksat: float
        @param imd: Initial moisture deficit.
        @type imd: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def get_exfil_params(self, idx: Union[int, str]) -> Tuple[float, float, float]:
        """Return exfiltration parameters for a storage node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Tuple of C{(suction, ksat, imd)}.
        @rtype: Tuple[float, float, float]
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    # ====================================================================
    # Outfall node methods
    # ====================================================================

    def set_outfall_type(self, idx: Union[int, str], type: int) -> None:
        """Set the outfall type for an outfall node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param type: Outfall type code.
        @type type: int
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def get_outfall_type(self, idx: Union[int, str]) -> int:
        """Return the outfall type code for an outfall node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Outfall type code.
        @rtype: int
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def set_outfall_stage(self, idx: Union[int, str], stage: float) -> None:
        """Set a fixed stage for an outfall node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param stage: Fixed stage value (project length units).
        @type stage: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def set_outfall_tidal(self, idx: Union[int, str], curve_idx: int) -> None:
        """Assign a tidal curve to an outfall node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param curve_idx: Tidal curve index.
        @type curve_idx: int
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def set_outfall_timeseries(self, idx: Union[int, str], ts_idx: int) -> None:
        """Assign a time series to an outfall node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param ts_idx: Time series index.
        @type ts_idx: int
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def get_outfall_param(self, idx: Union[int, str]) -> float:
        """Return the outfall parameter value for an outfall node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Outfall parameter value.
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def set_outfall_flap_gate(self, idx: Union[int, str], has_gate: bool) -> None:
        """Set whether an outfall node has a flap gate.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param has_gate: C{True} if the outfall has a flap gate.
        @type has_gate: bool
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def get_outfall_flap_gate(self, idx: Union[int, str]) -> bool:
        """Return whether an outfall node has a flap gate.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: C{True} if the outfall has a flap gate.
        @rtype: bool
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def set_outfall_route_to(self, idx: Union[int, str], subcatch_idx: int) -> None:
        """Set the subcatchment to route outfall discharge to.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param subcatch_idx: Subcatchment index (C{-1} = no routing).
        @type subcatch_idx: int
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def get_outfall_route_to(self, idx: Union[int, str]) -> int:
        """Get the subcatchment index outfall discharge is routed to.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Subcatchment index (C{-1} = no routing).
        @rtype: int
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    # ====================================================================
    # Statistics
    # ====================================================================

    def get_stat_max_depth(self, idx: Union[int, str]) -> float:
        """Return the maximum depth recorded at a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Maximum depth (project length units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def get_stat_max_overflow(self, idx: Union[int, str]) -> float:
        """Return the maximum overflow rate recorded at a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Maximum overflow rate (project flow units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def get_stat_vol_flooded(self, idx: Union[int, str]) -> float:
        """Return the total volume flooded at a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Volume flooded (project volume units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    def get_stat_time_flooded(self, idx: Union[int, str]) -> float:
        """Return the total time a node was flooded.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Time flooded (hours).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    # ====================================================================
    # Depth from volume (inverse)
    # ====================================================================

    def get_depth_from_volume(self, idx: Union[int, str], volume: float) -> float:
        """Compute depth from volume (inverse of volume-depth relationship).

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param volume: Volume (project volume units).
        @type volume: float
        @return: Depth (project length units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        ...

    # ====================================================================
    # Bulk array access (numpy)
    # ====================================================================

    def get_depths_bulk(self) -> npt.NDArray[np.float64]:
        """Return all node depths as a NumPy array.

        Uses the bulk C API for a single C{memcpy} -- much faster than
        calling L{get_depth} in a loop.

        @return: Array of shape C{(n_nodes,)} with dtype C{float64}.
        @rtype: numpy.typing.NDArray[numpy.float64]
        """
        ...

    def get_heads_bulk(self) -> npt.NDArray[np.float64]:
        """Return all node heads as a NumPy array.

        @return: Array of shape C{(n_nodes,)} with dtype C{float64}.
        @rtype: numpy.typing.NDArray[numpy.float64]
        """
        ...

    def set_depths_bulk(self, values: npt.NDArray[np.float64]) -> None:
        """Set all node depths from a NumPy array.

        @param values: Array of shape C{(n_nodes,)} with dtype C{float64}.
        @type values: numpy.typing.NDArray[numpy.float64]
        """
        ...

    def get_inflows_bulk(self) -> npt.NDArray[np.float64]:
        """Return all node total inflows as a NumPy array.

        @return: Array of shape C{(n_nodes,)} with dtype C{float64}.
        @rtype: numpy.typing.NDArray[numpy.float64]
        """
        ...

    def get_overflows_bulk(self) -> npt.NDArray[np.float64]:
        """Return all node overflow rates as a NumPy array.

        @return: Array of shape C{(n_nodes,)} with dtype C{float64}.
        @rtype: numpy.typing.NDArray[numpy.float64]
        """
        ...

    def set_lat_inflows_bulk(self, values: npt.NDArray[np.float64]) -> None:
        """Set all node lateral inflows from a NumPy array.

        @param values: Array of shape C{(n_nodes,)} with dtype C{float64}.
        @type values: numpy.typing.NDArray[numpy.float64]
        """
        ...

    def get_quality_bulk(self, pollutant_idx: int) -> npt.NDArray[np.float64]:
        """Return all node concentrations for a pollutant as a NumPy array.

        @param pollutant_idx: Pollutant index.
        @type pollutant_idx: int
        @return: Array of shape C{(n_nodes,)} with dtype C{float64}.
        @rtype: numpy.typing.NDArray[numpy.float64]
        """
        ...
