"""Public passive snapshots and trusted material transport ownership contracts."""

import gc
import json
from concurrent.futures import ThreadPoolExecutor
from dataclasses import FrozenInstanceError
from pathlib import Path

import pytest

from openswmm.engine import (
    EngineError,
    HAS_2D,
    LifecycleError,
    PassiveAmount,
    PluginError,
    Solver,
    SplitPhase,
    SplitRole,
)


@pytest.fixture
def engines(tmp_path):
    owners = []
    source = Path(__file__).resolve().parents[3] / "tests/unit/engine/data/passive_exchange_c/positive.inp"

    def create():
        path = tmp_path / "native.inp"
        path.write_text(source.read_text())
        solver = Solver(path, tmp_path / f"peer-{len(owners)}.rpt", None)
        owners.append(solver)
        solver.open()
        return solver

    yield create
    for solver in reversed(owners):
        solver.destroy()


def test_passive_refusals_are_typed_and_views_expire():
    solver = Solver()
    solver.create()
    view = solver.passive_exchange
    try:
        with pytest.raises(EngineError, match="Capacity"):
            view.enable(0)
        with pytest.raises(EngineError, match="preflight"):
            view.enable(64, 2)
        for call in (
            view.status, lambda: view.pool(0), lambda: view.receipt(0),
            lambda: view.row(0, 0), lambda: view.leg(0), view.attempt,
            view.enable_checkpoint, lambda: view.enable(64),
        ):
            with pytest.raises(LifecycleError if HAS_2D else PluginError):
                call()
    finally:
        solver.destroy()
    with pytest.raises(EngineError):
        view.status()


@pytest.mark.skipif(not HAS_2D, reason="requires 2D")
def test_passive_snapshots_preserve_components_and_checkpoint(engines, tmp_path):
    solver = engines()
    solver.initialize()
    solver.start(False)
    exchange = solver.passive_exchange
    exchange.enable(64)
    exchange.enable_checkpoint()
    assert exchange.status().checkpoint_attached == 1
    initial = exchange.pool(0)
    assert isinstance(initial.roundoff, PassiveAmount)
    assert isinstance(initial.roundoff.components, tuple)
    with pytest.raises(FrozenInstanceError):
        initial.node = 2
    for _ in range(8):
        solver.step()
    status = exchange.status()
    assert status.receipt_count > 0
    assert status.leg_count > 0
    assert not status.failed
    receipt = exchange.receipt(0)
    assert receipt.row_count > 0
    original = exchange.row(0, 0)
    remaining = exchange.row(0, 0, remaining=1)
    assert original.point == remaining.point
    assert original.units == status.units
    assert all(isinstance(a.components, tuple) for a in (
        original.drain_water, original.spill_water, original.drain_amount, original.spill_amount,
    ))
    leg = exchange.leg(receipt.first_leg)
    assert leg.sealed and leg.applied
    assert leg.point == original.point
    assert exchange.attempt().duration_seconds >= 0
    before = exchange.status()
    for call in (lambda: exchange.pool(-1), lambda: exchange.receipt(2**64-1),
                 lambda: exchange.row(0, 0, remaining=2), lambda: exchange.leg(2**64-1)):
        with pytest.raises(EngineError):
            call()
        assert exchange.status() == before
    snapshot = tmp_path / "passive checkpoint.bin"
    solver.checkpoints.save(snapshot)
    restored = engines()
    restored.initialize()
    restored.start(False)
    restored.passive_exchange.enable(64)
    restored.passive_exchange.enable_checkpoint()
    restored.checkpoints.restore(snapshot)
    assert restored.passive_exchange.status() == exchange.status()
    assert restored.passive_exchange.pool(0) == exchange.pool(0)
    # A returned snapshot never aliases subsequently changing native storage.
    assert initial != exchange.pool(0)


@pytest.mark.skipif(not HAS_2D, reason="requires 2D")
def test_passive_inspection_refuses_inside_callback(engines):
    solver = engines()
    solver.initialize()
    solver.start(False)
    solver.passive_exchange.enable(64)
    seen = []

    def observe(*args):
        with pytest.raises(LifecycleError):
            solver.passive_exchange.status()
        seen.append(True)

    solver.set_step_begin_callback(observe)
    solver.step()
    assert seen


@pytest.mark.parametrize("text", ["", "{}\0", pytest.param("x" * (64 * 1024 * 1024 + 1), id="oversized")])
def test_material_rejects_invalid_packet_buffers(text):
    solver = Solver()
    solver.create()
    try:
        for method in (solver.material_split.decode, solver.material_split.copy_packet):
            with pytest.raises(ValueError, match="Material packet"):
                method(text)
    finally:
        solver.destroy()


def test_material_feature_and_lifecycle_errors():
    solver = Solver()
    solver.create()
    try:
        for call in (solver.material_split.validate_input,
                     lambda: solver.material_split.configure(SplitRole.NETWORK),
                     lambda: solver.material_split.set_transport(lambda stage, text: text),
                     lambda: solver.material_split.copy_packet("{}"),
                     lambda: solver.material_split.decode("{}")):
            with pytest.raises(EngineError):
                call()
        with pytest.raises(TypeError):
            solver.material_split.set_transport(None)
    finally:
        solver.destroy()


@pytest.fixture
def material_peers(engines):
    if not HAS_2D:
        pytest.skip("requires 2D and material components")
    network, surface = engines(), engines()
    try:
        network.material_split.validate_input()
    except PluginError:
        pytest.skip("requires material components")
    network.material_split.configure(SplitRole.NETWORK)
    surface.material_split.configure(SplitRole.SURFACE)
    surface.initialize()
    with surface.material_split.geometry() as frame:
        packet = surface.material_split.encode(frame)
    network.initialize()
    with network.material_split.decode(packet) as frame:
        network.material_split.apply_geometry(frame)
    phases = []

    def exchange(phase, request):
        phases.append(phase)
        # Codec copies must neither consume a frame nor grant native authority.
        text = network.material_split.encode(request)
        with surface.material_split.decode(text) as imported:
            with surface.material_split.evaluate(phase, imported) as reply:
                encoded = surface.material_split.encode(reply)
        return network.material_split.decode(encoded)

    network.material_split.set_callback(exchange)
    surface.start(False)
    network.start(False)
    return network, surface, phases


def test_material_transport_retains_text_and_holds_both_owner_guards(material_peers, engines):
    network, surface, phases = material_peers
    reference = engines()
    reference.initialize()
    reference.start(False)
    reference.passive_exchange.enable(64)
    stages = []

    def transport(stage, text):
        stages.append(stage)
        assert stage in (1, 2, 3)
        with pytest.raises(LifecycleError):
            surface.destroy()
        with ThreadPoolExecutor(max_workers=1) as pool:
            with pytest.raises(LifecycleError):
                pool.submit(lambda: surface.handle).result()
        # Construct fresh storage, reorder JSON, then canonicalize through C.
        value = json.loads(text)
        response = surface.material_split.copy_packet(json.dumps(dict(reversed(list(value.items())))))
        gc.collect()
        return response

    network.material_split.set_transport(transport)
    gc.collect()
    network.material_split.bind(surface, 64)
    for _ in range(8):
        reference.step()
        network.step()
        assert reference.nodes[0].volume == network.nodes[0].volume
        assert reference.nodes[0].quality(0) == network.nodes[0].quality(0)
    assert {1, 2, 3}.issubset(stages)
    assert SplitPhase.ADVANCE in phases
    with pytest.raises(LifecycleError):
        network.material_split.set_transport(transport)
    with pytest.raises(LifecycleError):
        network.material_split.bind(surface, 64)


@pytest.mark.parametrize("bad_reply", [None, "{}", "raised"])
def test_material_transport_exception_or_invalid_reply_stops_exchange(material_peers, bad_reply):
    network, surface, _ = material_peers

    def transport(stage, text):
        if bad_reply == "raised":
            raise LookupError("material callback failed")
        return bad_reply

    network.material_split.set_transport(transport)
    network.material_split.bind(surface, 64)
    expected = LookupError if bad_reply == "raised" else (TypeError if bad_reply is None else EngineError)
    with pytest.raises(expected):
        for _ in range(4):
            network.step()
