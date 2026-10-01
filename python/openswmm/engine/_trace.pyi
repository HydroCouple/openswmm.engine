# SPDX-License-Identifier: Apache-2.0
"""Offline flow tracing over completed output or supplied hydraulic averages.

All dimensional quantities are SI. Travel times are passage-weighted estimates
under frozen average flows, not particle arrival times. A handle owns its inputs
and supports the context-manager protocol.
"""
from __future__ import annotations
from dataclasses import dataclass
from ._enums import (NodeType, LinkType, TraceDirection, TraceNodeFlags,
                     TraceFlags, TraceTerminal, TraceStatus)

from os import PathLike
from typing import Callable, Iterable, Any

@dataclass(frozen=True)
class TraceNode:
    """Topology node; flags describe ponding and external exchanges."""
    id: str = ''
    type: NodeType = NodeType.JUNCTION
    flags: TraceNodeFlags = TraceNodeFlags(0)


@dataclass(frozen=True)
class TraceLink:
    """Topology link; endpoint indices refer to the supplied node order."""
    id: str = ''
    from_node: int = 0
    to_node: int = 0
    type: LinkType = LinkType.CONDUIT
    length_m: float = 0.0


@dataclass(frozen=True)
class TraceOptions:
    """Numerical thresholds in SI units; use FlowTracer.default_options for native defaults."""
    flow_epsilon_m3s: float = 1e-9
    velocity_epsilon_mps: float = 1e-6
    reversal_dominance: float = 0.8
    solver_tolerance: float = 1e-11
    max_iterations: int = 10000


@dataclass(frozen=True)
class TraceNodeAverage:
    """Immutable hydraulic snapshot in SI units."""
    volume_m3: float = 0.0
    inflow_m3s: float = 0.0
    lateral_in_m3s: float = 0.0
    withdrawal_m3s: float = 0.0
    overflow_m3s: float = 0.0
    outgoing_m3s: float = 0.0
    residence_s: float = 0.0
    first_volume_m3: float = 0.0
    last_volume_m3: float = 0.0
    flags: TraceFlags = TraceFlags(0)


@dataclass(frozen=True)
class TraceLinkAverage:
    """Immutable hydraulic snapshot in SI units."""
    net_flow_m3s: float = 0.0
    absolute_flow_m3s: float = 0.0
    absolute_velocity_mps: float = 0.0
    forward_volume_m3: float = 0.0
    reverse_volume_m3: float = 0.0
    travel_s: float = 0.0
    first_volume_m3: float = 0.0
    last_volume_m3: float = 0.0
    direction: int = 0
    flags: TraceFlags = TraceFlags(0)


@dataclass(frozen=True)
class TraceInfo:
    """Scan metadata; report dates use the native OLE Automation day representation."""
    schema_version: int = 0
    algorithm_version: int = 0
    node_count: int = 0
    link_count: int = 0
    periods: int = 0
    source_flow_units: int = 0
    cache_loaded: int = 0
    first_report_date: float = 0.0
    last_report_date: float = 0.0
    duration_s: float = 0.0


@dataclass(frozen=True)
class TraceValue:
    """Passage ratio and timing; NaN denotes unknown time or trapped circulation."""
    ratio: float = 0.0
    time_s: float = 0.0
    time_coverage: float = 0.0
    from_time_s: float = 0.0
    to_time_s: float = 0.0
    terminal_fraction: float = 0.0
    terminal_kind: TraceTerminal | None = None
    flags: TraceFlags = TraceFlags(0)


@dataclass(frozen=True)
class TraceSummary:
    """Accounting totals; index terminal fractions with TraceTerminal."""
    terminal: tuple[float, ...] = (0.0,) * 6
    accounting_error: float = 0.0
    solver_residual: float = 0.0
    boundary_in_m3: float = 0.0
    boundary_out_m3: float = 0.0
    lateral_in_m3: float = 0.0
    withdrawal_m3: float = 0.0
    known_loss_m3: float = 0.0
    storage_change_m3: float = 0.0
    partial_balance_residual_m3: float = 0.0
    cyclic: int = 0
    reached_nodes: int = 0
    reached_links: int = 0


@dataclass(frozen=True)
class TraceResult:
    """Results in topology order, with terminal and continuity accounting."""
    nodes: tuple[TraceValue, ...]
    links: tuple[TraceValue, ...]
    summary: TraceSummary


class TraceError(RuntimeError):
    code: TraceStatus
    def __init__(self, code: int, message: str) -> None: ...


class FlowTracer:
    """Independent tracing handle; close explicitly or use a with block."""
    def __init__(self, nodes: Iterable[TraceNode], links: Iterable[TraceLink], *, options: TraceOptions | None = None) -> None: ...
    @staticmethod
    def default_options() -> TraceOptions: ...
    def close(self) -> None: ...
    def __enter__(self) -> FlowTracer: ...
    def __exit__(self, *args: Any) -> bool: ...
    @property
    def node_ids(self) -> tuple[str, ...]: ...
    @property
    def link_ids(self) -> tuple[str, ...]: ...
    @property
    def topology(self) -> tuple[tuple[TraceNode, ...], tuple[TraceLink, ...]]: ...
    @property
    def info(self) -> TraceInfo: ...
    @property
    def averages(self) -> tuple[tuple[TraceNodeAverage, ...], tuple[TraceLinkAverage, ...]]: ...
    def set_averages(self, nodes: Iterable[TraceNodeAverage], links: Iterable[TraceLinkAverage], *, info: TraceInfo | None = None) -> None: ...
    def prepare(self, output_path: str | PathLike[str], *, cache_path: str | PathLike[str] | None = None, fingerprint: str | None = None, progress: Callable[[float, str], bool | None] | None = None) -> None: ...
    def estimate(self, seed: str | int, direction: TraceDirection = TraceDirection.DOWNSTREAM, *, progress: Callable[[float, str], bool | None] | None = None) -> TraceResult: ...
