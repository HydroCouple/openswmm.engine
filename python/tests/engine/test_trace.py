"""Public tracing bindings: native results, ownership, callbacks and file I/O."""
from concurrent.futures import ThreadPoolExecutor
from dataclasses import FrozenInstanceError
import hashlib
import math
import struct
import threading

import pytest
from openswmm.engine import (
    FlowTracer, TraceNode, TraceLink, TraceOptions, TraceNodeAverage,
    TraceLinkAverage, TraceInfo, TraceError, TraceStatus, TraceDirection,
    TraceFlags, TraceTerminal, NodeType,
)


def two_nodes():
    return FlowTracer([TraceNode('S'), TraceNode('O', NodeType.OUTFALL)],
                      [TraceLink('SO', 0, 1, length_m=30)])


def set_unit_flow(trace):
    trace.set_averages([TraceNodeAverage(lateral_in_m3s=1), TraceNodeAverage()],
                       [TraceLinkAverage(net_flow_m3s=1, absolute_flow_m3s=1,
                                         absolute_velocity_mps=1)])


def test_split_confluence_and_upstream():
    nodes = [TraceNode('S'), TraceNode('A'), TraceNode('B'), TraceNode('O', NodeType.OUTFALL)]
    links = [TraceLink('SA', 0, 1, length_m=10), TraceLink('SB', 0, 2, length_m=20),
             TraceLink('AO', 1, 3, length_m=10), TraceLink('BO', 2, 3, length_m=20)]
    with FlowTracer(nodes, links, options=FlowTracer.default_options()) as trace:
        assert trace.node_ids == ('S', 'A', 'B', 'O')
        assert trace.link_ids == ('SA', 'SB', 'AO', 'BO')
        assert trace.topology == (tuple(nodes), tuple(links))
        nodes.clear()  # The native handle owns its topology.
        trace.set_averages([TraceNodeAverage(lateral_in_m3s=10)] + [TraceNodeAverage()] * 3,
                          [TraceLinkAverage(net_flow_m3s=q, absolute_flow_m3s=q,
                                            absolute_velocity_mps=1) for q in (6, 4, 6, 4)],
                          info=TraceInfo(duration_s=120))
        assert trace.info.node_count == 4
        assert trace.info.duration_s == 120
        result = trace.estimate('S')
        assert [item.ratio for item in result.links[:2]] == pytest.approx([0.6, 0.4])
        assert result.links[0].terminal_kind is None
        assert result.nodes[3].time_s == pytest.approx(28)
        assert result.summary.terminal[TraceTerminal.OUTFALL] == pytest.approx(1)
        upstream = trace.estimate(3, TraceDirection.UPSTREAM)
        assert upstream.nodes[0].time_s == pytest.approx(28)
        assert upstream.summary.terminal[TraceTerminal.SOURCE] == pytest.approx(1)
        _, averages = trace.averages
        assert averages[0].travel_s == 10
    assert result.nodes[3].ratio == pytest.approx(1)  # snapshots survive close
    with pytest.raises(FrozenInstanceError):
        result.nodes[0].ratio = 0


def test_unknown_times_and_empty_links():
    with two_nodes() as trace:
        trace.set_averages([TraceNodeAverage()] * 2,
                          [TraceLinkAverage(net_flow_m3s=1, absolute_flow_m3s=1)])
        result = trace.estimate('S')
        assert math.isnan(result.links[0].time_s)
        assert result.links[0].flags & TraceFlags.UNKNOWN_TIME
    with FlowTracer([TraceNode('dry')], []) as trace:
        trace.set_averages([TraceNodeAverage()], [])
        assert trace.link_ids == ()
        assert trace.averages[1] == ()
        assert trace.topology[1] == ()
        assert trace.estimate('dry').summary.terminal[TraceTerminal.RETAINED] == 1


def test_cycle_ratios_preserve_repeat_passages():
    with FlowTracer([TraceNode('S'), TraceNode('A'), TraceNode('O', NodeType.OUTFALL)],
                    [TraceLink('SA', 0, 1, length_m=4), TraceLink('AS', 1, 0, length_m=6),
                     TraceLink('AO', 1, 2, length_m=2)]) as trace:
        trace.set_averages([TraceNodeAverage(lateral_in_m3s=1)] + [TraceNodeAverage()] * 2,
                          [TraceLinkAverage(net_flow_m3s=q, absolute_flow_m3s=q,
                                            absolute_velocity_mps=1) for q in (2, 1, 1)])
        result = trace.estimate('S')
        assert result.summary.cyclic
        assert result.links[0].ratio == pytest.approx(2)
        assert result.nodes[2].time_s == pytest.approx(16)


def test_validation_and_handle_lifetime():
    assert FlowTracer.default_options() == TraceOptions()
    for nodes, links, options in [([], [], None),
                                  ([TraceNode('S'), TraceNode('S')], [], None),
                                  ([TraceNode('S')], [TraceLink('L', 0, 5)], None),
                                  ([TraceNode('S')], [], TraceOptions(max_iterations=0))]:
        with pytest.raises(TraceError) as exc:
            FlowTracer(nodes, links, options=options)
        assert exc.value.code == TraceStatus.INVALID
    with pytest.raises(ValueError, match='NUL'):
        FlowTracer([TraceNode('bad\0id')], [])
    trace = two_nodes()
    with pytest.raises(TraceError):
        _ = trace.averages
    with pytest.raises(ValueError, match='row counts'):
        trace.set_averages([], [])
    set_unit_flow(trace)
    for seed, error in [(8, IndexError), (-1, IndexError), ('absent', KeyError), (0.5, TypeError)]:
        with pytest.raises(error):
            trace.estimate(seed)
    with pytest.raises(ValueError):
        trace.estimate(0, 5)
    with pytest.raises(TypeError):
        trace.estimate(0, progress=42)
    trace.close()
    trace.close()
    with pytest.raises(RuntimeError, match='closed'):
        _ = trace.info
    with pytest.raises(RuntimeError, match='closed'):
        trace.__enter__()


def test_callbacks_cancel_propagate_and_reject_reentry():
    with two_nodes() as trace:
        set_unit_flow(trace)
        with pytest.raises(TraceError) as exc:
            trace.estimate('S', progress=lambda fraction, stage: True)
        assert exc.value.code == TraceStatus.CANCELLED
        def fail(fraction, stage):
            raise ValueError('callback failed')
        with pytest.raises(ValueError, match='callback failed'):
            trace.estimate('S', progress=fail)
        with pytest.raises(RuntimeError, match='in use'):
            trace.estimate('S', progress=lambda fraction, stage: trace.close())
        assert trace.estimate('S').nodes[1].ratio == pytest.approx(1)


def test_handle_rejects_concurrent_access():
    entered, release = threading.Event(), threading.Event()
    def progress(fraction, stage):
        entered.set()
        assert release.wait(10)
    with two_nodes() as trace, ThreadPoolExecutor(max_workers=1) as pool:
        set_unit_flow(trace)
        future = pool.submit(trace.estimate, 'S', progress=progress)
        try:
            assert entered.wait(10)
            with pytest.raises(RuntimeError, match='in use'):
                trace.close()
            with pytest.raises(RuntimeError, match='in use'):
                _ = trace.info
        finally:
            release.set()
        assert future.result(timeout=10).nodes[1].ratio == pytest.approx(1)


def write_output(path, units=3):
    """Small public-format output with reversed ID order and unequal periods."""
    with path.open('wb') as f:
        def integer(value): f.write(struct.pack('<i', value))
        def real(value): f.write(struct.pack('<f', value))
        def date(value): f.write(struct.pack('<d', value))
        for value in (516114522, 60000, units, 0, 2, 1, 0): integer(value)
        ids = f.tell()
        for value in (b'O', b'S', b'SO'):
            integer(len(value))
            f.write(value)
        inputs = f.tell()
        for value in (1, 1, 3, 0, 1, 2): integer(value)
        for node in range(2):
            integer(1 if node == 0 else 0)
            real(0)
            real(10)
        integer(5)
        for value in range(5): integer(value)
        integer(0)
        for _ in range(4): real(1)
        for count in (0, 6, 5, 0):
            integer(count)
            for value in range(count): integer(value)
        date(45000)
        integer(3600)
        output = f.tell()
        for hour, flow, volume in ((1, -2, 10), (2, 2, 20), (4, 6, 40)):
            date(45000 + hour / 24)
            for node in range(2):
                for value in (0, 0, volume if node else 0, flow if node else 0, abs(flow), 0): real(value)
            for value in (flow, 0, flow, volume, 0): real(value)
        for value in (ids, inputs, output, 3, 0, 516114522): integer(value)
    return path


def test_prepare_file_id_mapping_errors_and_cancellation(tmp_path):
    path = write_output(tmp_path / 'hydraulics.out')
    with two_nodes() as trace:
        seen = []
        trace.prepare(path, progress=lambda fraction, stage: seen.append((fraction, stage)))
        assert seen
        assert trace.info.periods == 3
        assert trace.info.duration_s == pytest.approx(10800)
        nodes, links = trace.averages
        assert nodes[0].volume_m3 == pytest.approx(25)
        assert links[0].net_flow_m3s == pytest.approx(8 / 3)
        assert links[0].travel_s == pytest.approx(10)
        # Mean withdrawal is 1/6 m3/s while net link flow is 8/3 m3/s.
        result = trace.estimate('S')
        assert result.nodes[1].ratio == pytest.approx(16 / 17)
        assert result.summary.terminal[TraceTerminal.LOSS] == pytest.approx(1 / 17)
        with pytest.raises(TraceError) as exc:
            trace.prepare(path, progress=lambda fraction, stage: True)
        assert exc.value.code == TraceStatus.CANCELLED
        with pytest.raises(TraceError) as exc:
            trace.prepare(tmp_path / 'absent.out')
        assert exc.value.code == TraceStatus.IO
        with pytest.raises(ValueError, match='fingerprint'):
            trace.prepare(path, cache_path=tmp_path / 'cache.h5')
    with FlowTracer([TraceNode('missing'), TraceNode('O', NodeType.OUTFALL)],
                    [TraceLink('SO', 0, 1, length_m=30)]) as trace:
        with pytest.raises(TraceError) as exc:
            trace.prepare(path)
        assert exc.value.code == TraceStatus.MISMATCH


def test_output_cache_and_restoring_averages(tmp_path):
    path = write_output(tmp_path / 'hydraulics.out')
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    cache = tmp_path / 'trace.h5'
    with two_nodes() as trace:
        try:
            trace.prepare(path, cache_path=cache, fingerprint=digest)
        except TraceError as exc:
            if exc.code == TraceStatus.NO_HDF5:
                pytest.skip('engine built without HDF5 caching')
            raise
        assert trace.info.cache_loaded == 0
        trace.prepare(path, cache_path=cache, fingerprint=digest)
        assert trace.info.cache_loaded == 1
        nodes, links = trace.averages
        info = trace.info
        expected = trace.estimate('S')
    with two_nodes() as restored:
        restored.set_averages(nodes, links, info=info)
        assert restored.estimate('S').nodes[1].time_s == pytest.approx(expected.nodes[1].time_s)
        us_path = write_output(tmp_path / 'us.out', units=0)
        restored.prepare(us_path)
        assert restored.averages[1][0].absolute_flow_m3s == pytest.approx(3 * 0.028316846592)
