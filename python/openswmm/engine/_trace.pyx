# SPDX-License-Identifier: Apache-2.0
"""Offline flow tracing over completed output or supplied hydraulic averages.

All dimensional quantities are SI. Travel times are passage-weighted estimates
under frozen average flows, not particle arrival times. A handle owns its inputs
and supports the context-manager protocol.
"""
from dataclasses import dataclass
from ._enums import (NodeType, LinkType, TraceDirection, TraceNodeFlags,
                     TraceFlags, TraceTerminal, TraceStatus)

from libcpp.vector cimport vector
import os

cdef extern from "openswmm/engine/openswmm_trace.h" nogil:
    ctypedef void* SWMM_Trace
    ctypedef int (*SWMM_TraceProgress)(double, const char*, void*) noexcept
    ctypedef struct SWMM_TraceNodeInput:
        const char * id
        int type
        int flags
    ctypedef struct SWMM_TraceLinkInput:
        const char * id
        int from_node
        int to_node
        int type
        double length_m
    ctypedef struct SWMM_TraceOptions:
        double flow_epsilon_m3s
        double velocity_epsilon_mps
        double reversal_dominance
        double solver_tolerance
        int max_iterations
    ctypedef struct SWMM_TraceNodeAverage:
        double volume_m3
        double inflow_m3s
        double lateral_in_m3s
        double withdrawal_m3s
        double overflow_m3s
        double outgoing_m3s
        double residence_s
        double first_volume_m3
        double last_volume_m3
        int flags
    ctypedef struct SWMM_TraceLinkAverage:
        double net_flow_m3s
        double absolute_flow_m3s
        double absolute_velocity_mps
        double forward_volume_m3
        double reverse_volume_m3
        double travel_s
        double first_volume_m3
        double last_volume_m3
        int direction
        int flags
    ctypedef struct SWMM_TraceInfo:
        int schema_version
        int algorithm_version
        int node_count
        int link_count
        int periods
        int source_flow_units
        int cache_loaded
        double first_report_date
        double last_report_date
        double duration_s
    ctypedef struct SWMM_TraceValue:
        double ratio
        double time_s
        double time_coverage
        double from_time_s
        double to_time_s
        double terminal_fraction
        int terminal_kind
        int flags
    ctypedef struct SWMM_TraceSummary:
        double terminal[6]
        double accounting_error
        double solver_residual
        double boundary_in_m3
        double boundary_out_m3
        double lateral_in_m3
        double withdrawal_m3
        double known_loss_m3
        double storage_change_m3
        double partial_balance_residual_m3
        int cyclic
        int reached_nodes
        int reached_links
    void swmm_trace_default_options(SWMM_TraceOptions *options)
    int swmm_trace_create(const SWMM_TraceNodeInput *nodes, int node_count, const SWMM_TraceLinkInput *links, int link_count, const SWMM_TraceOptions *options, SWMM_Trace *handle)
    void swmm_trace_close(SWMM_Trace handle)
    const char *swmm_trace_error(SWMM_Trace handle)
    int swmm_trace_prepare(SWMM_Trace handle, const char *output_path, const char *cache_path, const char *fingerprint, SWMM_TraceProgress progress, void *user)
    int swmm_trace_set_averages(SWMM_Trace handle, const SWMM_TraceNodeAverage *nodes, int node_count, const SWMM_TraceLinkAverage *links, int link_count, const SWMM_TraceInfo *info)
    int swmm_trace_get_info(SWMM_Trace handle, SWMM_TraceInfo *info)
    int swmm_trace_get_averages(SWMM_Trace handle, SWMM_TraceNodeAverage *nodes, int node_capacity, SWMM_TraceLinkAverage *links, int link_capacity)
    const char *swmm_trace_node_id(SWMM_Trace handle, int index)
    const char *swmm_trace_link_id(SWMM_Trace handle, int index)
    int swmm_trace_get_topology(SWMM_Trace handle, SWMM_TraceNodeInput *nodes, int node_capacity, SWMM_TraceLinkInput *links, int link_capacity)
    int swmm_trace_estimate(SWMM_Trace handle, int direction, int seed, SWMM_TraceValue *nodes, int node_capacity, SWMM_TraceValue *links, int link_capacity, SWMM_TraceSummary *summary, SWMM_TraceProgress progress, void *user)


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


cdef bytes _text(value):
    cdef bytes encoded = str(value).encode('utf-8')
    if b'\0' in encoded:
        raise ValueError("Trace strings cannot contain NUL characters")
    return encoded


cdef int _progress(double fraction, const char* stage, void* user) noexcept with gil:
    context = <object>user
    try:
        return bool(context[0](fraction, stage.decode('utf-8') if stage != NULL else ''))
    except BaseException as exc:
        # Never let Python exceptions escape through the native callback ABI.
        context[1] = exc
        return 1


class TraceError(RuntimeError):
    """Native tracing failure; code is a TraceStatus, separate from engine errors."""
    def __init__(self, code, message):
        self.code = TraceStatus(code)
        super().__init__(message or f"Flow tracing failed ({self.code.name})")


cdef object _nodeinput(SWMM_TraceNodeInput value):
    return TraceNode(
        value.id.decode('utf-8'),
        NodeType(value.type),
        TraceNodeFlags(value.flags),
    )


cdef object _linkinput(SWMM_TraceLinkInput value):
    return TraceLink(
        value.id.decode('utf-8'),
        value.from_node,
        value.to_node,
        LinkType(value.type),
        value.length_m,
    )


cdef object _options(SWMM_TraceOptions value):
    return TraceOptions(
        value.flow_epsilon_m3s,
        value.velocity_epsilon_mps,
        value.reversal_dominance,
        value.solver_tolerance,
        value.max_iterations,
    )


cdef object _nodeaverage(SWMM_TraceNodeAverage value):
    return TraceNodeAverage(
        value.volume_m3,
        value.inflow_m3s,
        value.lateral_in_m3s,
        value.withdrawal_m3s,
        value.overflow_m3s,
        value.outgoing_m3s,
        value.residence_s,
        value.first_volume_m3,
        value.last_volume_m3,
        TraceFlags(value.flags),
    )


cdef object _linkaverage(SWMM_TraceLinkAverage value):
    return TraceLinkAverage(
        value.net_flow_m3s,
        value.absolute_flow_m3s,
        value.absolute_velocity_mps,
        value.forward_volume_m3,
        value.reverse_volume_m3,
        value.travel_s,
        value.first_volume_m3,
        value.last_volume_m3,
        value.direction,
        TraceFlags(value.flags),
    )


cdef object _info(SWMM_TraceInfo value):
    return TraceInfo(
        value.schema_version,
        value.algorithm_version,
        value.node_count,
        value.link_count,
        value.periods,
        value.source_flow_units,
        value.cache_loaded,
        value.first_report_date,
        value.last_report_date,
        value.duration_s,
    )


cdef object _value(SWMM_TraceValue value):
    return TraceValue(
        value.ratio,
        value.time_s,
        value.time_coverage,
        value.from_time_s,
        value.to_time_s,
        value.terminal_fraction,
        None if value.terminal_kind < 0 else TraceTerminal(value.terminal_kind),
        TraceFlags(value.flags),
    )


cdef object _summary(SWMM_TraceSummary value):
    return TraceSummary(
        tuple(value.terminal[i] for i in range(6)),
        value.accounting_error,
        value.solver_residual,
        value.boundary_in_m3,
        value.boundary_out_m3,
        value.lateral_in_m3,
        value.withdrawal_m3,
        value.known_loss_m3,
        value.storage_change_m3,
        value.partial_balance_residual_m3,
        value.cyclic,
        value.reached_nodes,
        value.reached_links,
    )


cdef class FlowTracer:
    """Independent, context-managed tracing over frozen hydraulic averages.

    Methods reject access while prepare/estimate is running, including access
    from a progress callback. Long native operations release the GIL. Callback
    exceptions propagate after the native operation has stopped safely.
    """
    cdef SWMM_Trace _handle
    cdef int _nn, _nl
    cdef bint _busy

    def __init__(self, nodes, links, *, options=None):
        cdef vector[SWMM_TraceNodeInput] ns
        cdef vector[SWMM_TraceLinkInput] ls
        cdef SWMM_TraceOptions opts
        cdef int i
        if self._handle != NULL:
            raise RuntimeError("FlowTracer is already initialized")
        nodes, links = tuple(nodes), tuple(links)
        self._nn, self._nl = len(nodes), len(links)
        ns.resize(self._nn)
        ls.resize(self._nl)
        # Keep encoded IDs alive until the C API has copied the topology.
        node_ids = [_text(row.id) for row in nodes]
        link_ids = [_text(row.id) for row in links]
        for i, row in enumerate(nodes):
            ns[i].id = node_ids[i]
            ns[i].type = row.type
            ns[i].flags = row.flags
        for i, row in enumerate(links):
            ls[i].id = link_ids[i]
            ls[i].from_node = row.from_node
            ls[i].to_node = row.to_node
            ls[i].type = row.type
            ls[i].length_m = row.length_m
        swmm_trace_default_options(&opts)
        if options is not None:
            opts.flow_epsilon_m3s = options.flow_epsilon_m3s
            opts.velocity_epsilon_mps = options.velocity_epsilon_mps
            opts.reversal_dominance = options.reversal_dominance
            opts.solver_tolerance = options.solver_tolerance
            opts.max_iterations = options.max_iterations
        self._check(swmm_trace_create(ns.data(), self._nn, ls.data(), self._nl,
                                      &opts, &self._handle))

    cdef SWMM_Trace _h(self) except NULL:
        if self._busy:
            raise RuntimeError("FlowTracer is in use by another operation")
        if self._handle == NULL:
            raise RuntimeError("FlowTracer is closed")
        return self._handle

    cdef void _check(self, int status) except *:
        cdef const char* message
        if status:
            message = swmm_trace_error(self._handle)
            raise TraceError(status, message.decode('utf-8', 'replace') if message != NULL else '')

    @staticmethod
    def default_options():
        """Return the native numerical defaults in SI units."""
        cdef SWMM_TraceOptions value
        swmm_trace_default_options(&value)
        return _options(value)

    def close(self):
        """Release this handle; repeated closes are harmless."""
        if self._busy:
            raise RuntimeError("FlowTracer is in use by another operation")
        if self._handle != NULL:
            swmm_trace_close(self._handle)
            self._handle = NULL

    def __dealloc__(self):
        if self._handle != NULL:
            swmm_trace_close(self._handle)

    def __enter__(self):
        self._h()
        return self

    def __exit__(self, *args):
        self.close()
        return False

    @property
    def node_ids(self):
        """Node IDs in the original topology order."""
        cdef SWMM_Trace h = self._h()
        return tuple(swmm_trace_node_id(h, i).decode('utf-8') for i in range(self._nn))

    @property
    def link_ids(self):
        """Link IDs in the original topology order."""
        cdef SWMM_Trace h = self._h()
        return tuple(swmm_trace_link_id(h, i).decode('utf-8') for i in range(self._nl))

    @property
    def topology(self):
        """Return owned node and link snapshots in input order."""
        cdef vector[SWMM_TraceNodeInput] nodes
        cdef vector[SWMM_TraceLinkInput] links
        nodes.resize(self._nn)
        links.resize(self._nl)
        self._check(swmm_trace_get_topology(self._h(), nodes.data(), self._nn,
                                            links.data(), self._nl))
        return (tuple(_nodeinput(nodes[i]) for i in range(self._nn)),
                tuple(_linkinput(links[i]) for i in range(self._nl)))

    @property
    def info(self):
        """Return scan metadata, including topology counts and cache status."""
        cdef SWMM_TraceInfo value
        self._check(swmm_trace_get_info(self._h(), &value))
        return _info(value)

    @property
    def averages(self):
        """Return prepared hydraulic averages and derived delays in SI units."""
        cdef vector[SWMM_TraceNodeAverage] nodes
        cdef vector[SWMM_TraceLinkAverage] links
        nodes.resize(self._nn)
        links.resize(self._nl)
        self._check(swmm_trace_get_averages(self._h(), nodes.data(), self._nn,
                                            links.data(), self._nl))
        return (tuple(_nodeaverage(nodes[i]) for i in range(self._nn)),
                tuple(_linkaverage(links[i]) for i in range(self._nl)))

    def set_averages(self, nodes, links, *, info=None):
        """Restore averages in topology order; derived delays are recomputed."""
        cdef vector[SWMM_TraceNodeAverage] ns
        cdef vector[SWMM_TraceLinkAverage] ls
        cdef SWMM_TraceInfo meta
        cdef SWMM_TraceInfo* info_ptr = NULL
        cdef int i
        self._h()
        nodes, links = tuple(nodes), tuple(links)
        if len(nodes) != self._nn or len(links) != self._nl:
            raise ValueError("Average row counts must match the topology")
        ns.resize(self._nn)
        ls.resize(self._nl)
        for i, row in enumerate(nodes):
            ns[i].volume_m3 = row.volume_m3
            ns[i].inflow_m3s = row.inflow_m3s
            ns[i].lateral_in_m3s = row.lateral_in_m3s
            ns[i].withdrawal_m3s = row.withdrawal_m3s
            ns[i].overflow_m3s = row.overflow_m3s
            ns[i].outgoing_m3s = row.outgoing_m3s
            ns[i].residence_s = row.residence_s
            ns[i].first_volume_m3 = row.first_volume_m3
            ns[i].last_volume_m3 = row.last_volume_m3
            ns[i].flags = row.flags
        for i, row in enumerate(links):
            ls[i].net_flow_m3s = row.net_flow_m3s
            ls[i].absolute_flow_m3s = row.absolute_flow_m3s
            ls[i].absolute_velocity_mps = row.absolute_velocity_mps
            ls[i].forward_volume_m3 = row.forward_volume_m3
            ls[i].reverse_volume_m3 = row.reverse_volume_m3
            ls[i].travel_s = row.travel_s
            ls[i].first_volume_m3 = row.first_volume_m3
            ls[i].last_volume_m3 = row.last_volume_m3
            ls[i].direction = row.direction
            ls[i].flags = row.flags
        if info is not None:
            meta.schema_version = info.schema_version
            meta.algorithm_version = info.algorithm_version
            meta.node_count = info.node_count
            meta.link_count = info.link_count
            meta.periods = info.periods
            meta.source_flow_units = info.source_flow_units
            meta.cache_loaded = info.cache_loaded
            meta.first_report_date = info.first_report_date
            meta.last_report_date = info.last_report_date
            meta.duration_s = info.duration_s
            info_ptr = &meta
        self._check(swmm_trace_set_averages(self._h(), ns.data(), self._nn,
                                            ls.data(), self._nl, info_ptr))

    def prepare(self, output_path, *, cache_path=None, fingerprint=None, progress=None):
        """Read a completed output, matching topology by ID rather than row order.

        A cache requires HDF5 and a caller-verified content digest in fingerprint
        (e.g. SHA-256 of the output bytes). It must change whenever the output
        changes; a path or timestamp is insufficient. The optional progress
        callback receives (fraction, stage); returning true cancels the scan.
        """
        cdef bytes output = _text(os.fspath(output_path))
        cdef bytes cache = b'' if cache_path is None else _text(os.fspath(cache_path))
        cdef bytes digest = b'' if fingerprint is None else _text(fingerprint)
        cdef const char* output_ptr = output
        cdef const char* cache_ptr = cache
        cdef const char* digest_ptr = digest
        cdef SWMM_TraceProgress callback = NULL
        cdef void* user = NULL
        cdef SWMM_Trace h = self._h()
        cdef int status
        if cache and not digest:
            raise ValueError("A cache requires a verified output content fingerprint")
        if progress is not None and not callable(progress):
            raise TypeError("progress must be callable")
        context = [progress, None]
        if progress is not None:
            callback, user = _progress, <void*>context
        h = self._h()
        self._busy = True
        try:
            with nogil:
                status = swmm_trace_prepare(h, output_ptr, cache_ptr, digest_ptr, callback, user)
        finally:
            self._busy = False
        if context[1] is not None:
            raise context[1]
        self._check(status)

    def estimate(self, seed, direction=TraceDirection.DOWNSTREAM, *, progress=None):
        """Trace upstream or downstream from a node ID or zero-based index.

        Results preserve NaN for unknown times and repeat passage ratios above
        one for cycles. Returning true from progress cancels the operation.
        """
        cdef vector[SWMM_TraceValue] nodes
        cdef vector[SWMM_TraceValue] links
        cdef SWMM_TraceSummary summary
        cdef SWMM_TraceProgress callback = NULL
        cdef void* user = NULL
        cdef int index, native_direction, status
        cdef SWMM_Trace h = self._h()
        if isinstance(seed, str):
            try:
                index = self.node_ids.index(seed)
            except ValueError:
                raise KeyError(seed) from None
        else:
            import operator
            index = operator.index(seed)
        native_direction = int(TraceDirection(direction))
        if not 0 <= index < self._nn:
            raise IndexError("Trace seed index is outside the topology")
        if progress is not None and not callable(progress):
            raise TypeError("progress must be callable")
        nodes.resize(self._nn)
        links.resize(self._nl)
        context = [progress, None]
        if progress is not None:
            callback, user = _progress, <void*>context
        h = self._h()
        self._busy = True
        try:
            with nogil:
                status = swmm_trace_estimate(h, native_direction, index,
                    nodes.data(), self._nn, links.data(), self._nl, &summary, callback, user)
        finally:
            self._busy = False
        if context[1] is not None:
            raise context[1]
        self._check(status)
        return TraceResult(tuple(_value(nodes[i]) for i in range(self._nn)),
                           tuple(_value(links[i]) for i in range(self._nl)), _summary(summary))
