from dataclasses import dataclass
from ._access import EngineView
from ._solver import Solver

@dataclass(frozen=True)
class PassiveAmount:
    components: tuple[float, ...]

@dataclass(frozen=True)
class PassiveStatus:
    receipt_count: int
    leg_count: int
    unsealed_leg_count: int
    pollutant: int
    units: int
    failed: int
    window_open: int
    delivery_frozen: int
    history_preflight: int
    checkpoint_attached: int
    history_refused: int
    elapsed_ms: float
    delivery_seconds: float

@dataclass(frozen=True)
class PassivePool:
    node: int
    units: int
    settled_volume_ft3: float
    settled_depth_ft: float
    settled_concentration: float
    settled_physical_amount: float
    hydraulic_volume_ft3: float
    roundoff: PassiveAmount
    pending_drain: PassiveAmount
    pending_spill: PassiveAmount

@dataclass(frozen=True)
class PassiveReceipt:
    id: int
    row_count: int
    first_leg: int
    leg_count: int
    generation: int
    phase: int
    capture_ms: float
    start_ms: float
    end_ms: float
    eligible_ms: float
    publication_delivery_seconds: float

@dataclass(frozen=True)
class PassiveRow:
    point: int
    node: int
    units: int
    drain_water: PassiveAmount
    spill_water: PassiveAmount
    drain_amount: PassiveAmount
    spill_amount: PassiveAmount

@dataclass(frozen=True)
class PassiveLeg:
    point: int
    cell: int
    node: int
    drain: int
    applied: int
    sealed: int
    phase: int
    water_m3: float
    source_amount: float
    water_ft3: float
    native_amount: float
    source_before: float
    source_after: float
    sampled_concentration: float
    volume_before_m3: float
    volume_after_m3: float
    start_ms: float
    end_ms: float
    offset_seconds: float
    duration_seconds: float
    remaining_water_ft3: float
    remaining_native_amount: float

class PassiveExchange(EngineView):
    def __init__(self, owner: Solver) -> None: ...
    def enable(self, capacity: int = ..., preflight: int = ...) -> None: ...
    def enable_checkpoint(self) -> None: ...
    def status(self) -> PassiveStatus: ...
    def pool(self, node: int) -> PassivePool: ...
    def receipt(self, receipt: int) -> PassiveReceipt: ...
    def row(self, receipt: int, row: int, remaining: int = ...) -> PassiveRow: ...
    def leg(self, leg: int) -> PassiveLeg: ...
    def attempt(self) -> PassiveLeg: ...
