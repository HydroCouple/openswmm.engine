"""Native peer ownership, callback failures and UTF-8 phase packet contracts."""

import gc
from concurrent.futures import ThreadPoolExecutor
from datetime import timedelta

import pytest

from openswmm.engine import (
    EngineError,
    HAS_2D,
    LifecycleError,
    PluginError,
    Solver,
    SplitFrame,
    SplitParticipant,
    SplitPhase,
    SplitRole,
    StaleObjectError,
)
from tests.engine.test_lid_nodes import DECK as CORE_DECK

DECK = """[OPTIONS]
FLOW_UNITS CMS
FLOW_ROUTING DYNWAVE
START_DATE 01/01/2026
START_TIME 00:00:00
END_DATE 01/01/2026
END_TIME 00:00:02
ROUTING_STEP .31
REPORT_STEP 00:00:01
IGNORE_QUALITY YES
ALLOW_PONDING YES
MIN_SURFAREA 1
MINIMUM_STEP .001
VARIABLE_STEP 0
[JUNCTIONS]
J 0 1 .1 2 10
[OUTFALLS]
O 0 FIXED 0 NO
[CONDUITS]
C J O 30 .013 0 0
[XSECTIONS]
C CIRCULAR 1 0 0 0 1
[COORDINATES]
J 2 2
O 12 2
[2D_OPTIONS]
BACKEND CPU
MAX_TIMESTEP .2
RECONSTRUCTION_ORDER 1
REPORT_2D YES
REPORT_2D_STEP 1
TRANSPORT_POLLUTANTS NO
TRANSPORT_MSX NO
TRANSPORT_AGE NO
TRANSPORT_TEMPERATURE NO
BED_HEAT_MODEL NONE
COUPLING_SYNC .7
[2D_VERTICES]
0 0 1
10 0 1
10 10 1
0 10 1
[2D_TRIANGLES]
0 1 2 .03 .2
0 2 3 .03 .1
[2D_TRIANGLE_NODE_MAP]
0 J .6 .02
"""


@pytest.fixture
def opened(tmp_path):
    solvers = []

    def create(deck=DECK, *, binary=True):
        path = tmp_path / "model.inp"
        path.write_text(deck)
        solver = Solver(
            path,
            tmp_path / f"peer-{len(solvers)}.rpt",
            tmp_path / f"peer-{len(solvers)}.out" if binary else None,
        )
        solver.open()
        solvers.append(solver)
        return solver

    yield create
    for solver in reversed(solvers):
        solver.destroy()


@pytest.fixture
def peers(opened):
    if not HAS_2D:
        pytest.skip("requires 2D engine")
    network, surface = opened(), opened(binary=False)
    network.split.validate_input()
    network.split.configure(SplitRole.NETWORK)
    surface.split.configure(SplitRole.SURFACE)
    surface.initialize()
    frame = surface.split.geometry()
    network.initialize()
    network.split.apply_geometry(frame)
    return network, surface, frame


def test_geometry_is_owned_closed_once_and_input_checks_are_native(peers):
    network, surface, frame = peers
    assert frame.phase == 0
    assert frame.time[2] == 0
    assert frame.node(0)["depth_m"] >= 0
    with pytest.raises(EngineError):
        frame.node(-1)
    with pytest.raises(EngineError):
        frame.source(0)  # unavailable outside a reviewed source profile
    with pytest.raises(EngineError):
        frame.outfall_return(0)
    frame.close()
    frame.close()
    with pytest.raises(ValueError):
        network.split.apply_geometry(frame)
    with surface.split.geometry() as second:
        assert second.node(0)["head_m"] >= 0
    with pytest.raises(ValueError):
        _ = second.time


def test_callback_request_expires_reply_is_transferred_and_cross_thread_use_is_refused(
    peers,
):
    network, surface, geometry = peers
    requests, replies, phases = [], [], []

    def exchange(phase, request):
        phases.append(phase)
        requests.append(request)
        assert request.time[2] > 0 or phase == SplitPhase.FINISH
        with pytest.raises(ValueError):
            request.close()
        with ThreadPoolExecutor(max_workers=1) as pool:
            with pytest.raises(RuntimeError):
                pool.submit(lambda: request.time).result()
        reply = surface.split.evaluate(phase, request)
        replies.append(reply)
        return reply

    network.split.set_callback(exchange)
    network.split.set_callback(None)
    network.split.set_callback(exchange)
    gc.collect()  # the network, rather than a temporary view, retains the callback
    surface.start(False)
    network.start(False)
    for _ in range(100):
        if network.step() == timedelta(0):
            break
    else:
        pytest.fail("split run did not finish")
    assert SplitPhase.ADVANCE in phases
    network.end()
    assert SplitPhase.FINISH in phases
    for value in requests + replies:
        with pytest.raises(ValueError):
            _ = value.time
    for request in requests:
        with pytest.raises(ValueError):
            request.__init__(surface)
    geometry.close()


@pytest.mark.parametrize("failure", ["exception", "alias", "wrong_type"])
def test_failed_callback_never_unwinds_c_abi_and_preserves_native_lifecycle(
    peers, failure
):
    network, surface, geometry = peers

    def exchange(phase, request):
        if failure == "exception":
            raise LookupError("deliberate split callback failure")
        return request if failure == "alias" else None

    network.split.set_callback(exchange)
    surface.start(False)
    network.start(False)
    expected = {"exception": LookupError, "alias": ValueError, "wrong_type": TypeError}[
        failure
    ]
    with pytest.raises(expected):
        network.step()
    with pytest.raises(LifecycleError):
        network.split.set_callback(None)
    network.destroy()
    geometry.close()


def test_phase_observer_reentry_is_refused_and_python_errors_are_deferred(opened):
    solver = opened(CORE_DECK)
    observed = []

    def observe(phase, start, end):
        observed.append((phase, start, end))
        with pytest.raises(LifecycleError):
            solver.destroy()
        raise LookupError("phase observer failure")

    solver.set_phase_callback(observe)
    solver.initialize()
    solver.start(False)
    with pytest.raises(LookupError, match="phase observer failure"):
        solver.step()
    assert observed and observed[0][2] >= observed[0][1]
    solver.set_phase_callback(None)
    solver.step()
    with pytest.raises(TypeError):
        solver.set_phase_callback(object())


def test_utf8_packets_roundtrip_owned_frames_or_report_omitted_native_codec(
    peers, opened
):
    network, surface, geometry = peers
    try:
        text = surface.split.encode(geometry)
    except PluginError:
        with pytest.raises(PluginError):
            surface.split.decode("{}")
        pytest.skip("native packet codec not built")
    assert isinstance(text, str)
    receiver = opened()
    receiver.split.configure(SplitRole.NETWORK)
    receiver.initialize()
    with receiver.split.decode(text) as decoded:
        assert decoded.phase == geometry.phase
        assert decoded.time == geometry.time
        # Packets contain published phase quantities, not a full native node snapshot.
        with pytest.raises(EngineError):
            decoded.node(0)
        receiver.split.apply_geometry(decoded)
    with pytest.raises(EngineError):
        surface.split.decode('{"broken": "雨"}')
    with pytest.raises(ValueError):
        surface.split.decode(text + "\0")
    geometry.close()


def test_participant_views_expire_on_close_and_empty_frames_are_safe(peers):
    network, surface, frame = peers
    view = surface.split
    surface.close()
    with pytest.raises(StaleObjectError):
        view.geometry()
    frame.close()
    empty = SplitFrame()
    empty.close()
    with pytest.raises(ValueError):
        _ = empty.phase


def test_split_constructors_reject_non_solver_owners():
    with pytest.raises(TypeError):
        SplitFrame(object())
    with pytest.raises(TypeError):
        SplitParticipant(object())


def test_split_operations_reject_none_frames(opened):
    solver = opened(CORE_DECK)
    for invoke in [
        lambda: solver.split.apply_geometry(None),
        lambda: solver.split.evaluate(SplitPhase.ADVANCE, None),
        lambda: solver.split.encode(None),
    ]:
        with pytest.raises(TypeError):
            invoke()
