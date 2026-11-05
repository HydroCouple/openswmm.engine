"""Compile-time consumers of the complete editor and native-peer bindings."""

from openswmm.engine import Solver, SplitFrame, SplitParticipant, SplitPhase, SplitRole


def native_peers(network: Solver, surface: Solver) -> None:
    peer: SplitParticipant = surface.split
    peer.validate_input()
    peer.configure(SplitRole.SURFACE)
    frame: SplitFrame = peer.geometry()
    time: tuple[int, float, float] = frame.time
    phase: int = frame.phase
    node: dict[str, float] = frame.node(0)
    source: dict[str, float] = frame.source(0)
    returned: float = frame.outfall_return(0)
    packet: str = peer.encode(frame)
    with network.split.decode(packet) as decoded:
        network.split.apply_geometry(decoded)

    def exchange(kind: SplitPhase, request: SplitFrame) -> SplitFrame:
        return surface.split.evaluate(kind, request)

    network.split.set_callback(exchange)
    network.split.set_callback(None)

    def observe(kind: int, start: float, end: float) -> None:
        pass

    network.set_phase_callback(observe)
    network.set_phase_callback(None)
    frame.close()
    _ = time, phase, node, source, returned


def editor_records(solver: Solver) -> None:
    divider: dict[str, int | float] = solver.nodes["V"].divider.params
    solver.nodes["V"].divider.params = divider
    lids = solver.infrastructure.lids
    snapshot = lids.node_snapshot("S")
    proposal = lids.prepare_node("S", "Stack", initial_saturation=25, bottom=0)
    lids.validate_node("S", proposal)
    lids.replace_node("S", snapshot)
    record = lids.usage_record(0)
    lids.validate_usage(record)
    lids.replace_usages("A", [record])
    solver.infrastructure.inlet_usages.validate(
        solver.infrastructure.inlet_usages.get(0)
    )
    solver.surface2d.validate_node_coupling(0, 1, {"D": 0.24})
    solver.surface2d.replace_node_coupling(0, 1, authored=False)

    inlets = solver.infrastructure.inlets
    design = inlets.validate_design(inlets.get_design(0))
    inlets.set_design(0, design)
