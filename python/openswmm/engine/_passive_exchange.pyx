# SPDX-License-Identifier: Apache-2.0
"""Experimental integrated native passive exchange, with read-only V1 snapshots.

Amount components retain their canonical order and native concentration/volume
basis. Inspection copies carry no ownership or authority to fund a transfer.
Access is serialized with the solver; inspection from callbacks is refused.
"""
from dataclasses import dataclass
from ._access import EngineView
from ._solver cimport Solver
from ._exceptions import raise_for_code

cdef void _check_passive(int code) except *:
    if code:
        message = swmm_passive_exchange_last_error()
        raise_for_code(code, message.decode('utf-8', 'replace') if message != NULL else None)

@dataclass(frozen=True)
class PassiveAmount:
    """Exact sum of original ordered components; empty tuple represents zero."""
    components: tuple[float, ...]

cdef object _amount(SWMM_PassiveAmountV1 value):
    return PassiveAmount(tuple(value.components[i] for i in range(value.count)))

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

cdef object _status(SWMM_PassiveStatusV1 value):
    return PassiveStatus(
        receipt_count=value.receipt_count,
        leg_count=value.leg_count,
        unsealed_leg_count=value.unsealed_leg_count,
        pollutant=value.pollutant,
        units=value.units,
        failed=value.failed,
        window_open=value.window_open,
        delivery_frozen=value.delivery_frozen,
        history_preflight=value.history_preflight,
        checkpoint_attached=value.checkpoint_attached,
        history_refused=value.history_refused,
        elapsed_ms=value.elapsed_ms,
        delivery_seconds=value.delivery_seconds,
    )

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

cdef object _pool(SWMM_PassivePoolV1 value):
    return PassivePool(
        node=value.node,
        units=value.units,
        settled_volume_ft3=value.settled_volume_ft3,
        settled_depth_ft=value.settled_depth_ft,
        settled_concentration=value.settled_concentration,
        settled_physical_amount=value.settled_physical_amount,
        hydraulic_volume_ft3=value.hydraulic_volume_ft3,
        roundoff=_amount(value.roundoff),
        pending_drain=_amount(value.pending_drain),
        pending_spill=_amount(value.pending_spill),
    )

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

cdef object _receipt(SWMM_PassiveReceiptV1 value):
    return PassiveReceipt(
        id=value.id,
        row_count=value.row_count,
        first_leg=value.first_leg,
        leg_count=value.leg_count,
        generation=value.generation,
        phase=value.phase,
        capture_ms=value.capture_ms,
        start_ms=value.start_ms,
        end_ms=value.end_ms,
        eligible_ms=value.eligible_ms,
        publication_delivery_seconds=value.publication_delivery_seconds,
    )

@dataclass(frozen=True)
class PassiveRow:
    point: int
    node: int
    units: int
    drain_water: PassiveAmount
    spill_water: PassiveAmount
    drain_amount: PassiveAmount
    spill_amount: PassiveAmount

cdef object _row(SWMM_PassiveRowV1 value):
    return PassiveRow(
        point=value.point,
        node=value.node,
        units=value.units,
        drain_water=_amount(value.drain_water),
        spill_water=_amount(value.spill_water),
        drain_amount=_amount(value.drain_amount),
        spill_amount=_amount(value.spill_amount),
    )

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

cdef object _leg(SWMM_PassiveLegV1 value):
    return PassiveLeg(
        point=value.point,
        cell=value.cell,
        node=value.node,
        drain=value.drain,
        applied=value.applied,
        sealed=value.sealed,
        phase=value.phase,
        water_m3=value.water_m3,
        source_amount=value.source_amount,
        water_ft3=value.water_ft3,
        native_amount=value.native_amount,
        source_before=value.source_before,
        source_after=value.source_after,
        sampled_concentration=value.sampled_concentration,
        volume_before_m3=value.volume_before_m3,
        volume_after_m3=value.volume_after_m3,
        start_ms=value.start_ms,
        end_ms=value.end_ms,
        offset_seconds=value.offset_seconds,
        duration_seconds=value.duration_seconds,
        remaining_water_ft3=value.remaining_water_ft3,
        remaining_native_amount=value.remaining_native_amount,
    )

class PassiveExchange(EngineView):
    """Integrated reference profile exposed as Solver.passive_exchange.

    Enable once after start(False), before stepping. This is separate from
    material-split enrollment. Native eligibility and feature checks apply.
    """
    def __init__(self, owner):
        super().__init__(owner)

    def enable(self, uint64_t capacity=4096, int preflight=0):
        """Enroll the supported passive profile; capacity is 1..65536, preflight 0/1."""
        (<Solver>self._owner)._access.require_idle()
        with self._owner._operation():
            _check_passive(swmm_passive_exchange_enable(<SWMM_Engine><size_t>self._address(), capacity, preflight))

    def enable_checkpoint(self):
        """Attach numerical checkpoints at the initial enrolled boundary."""
        (<Solver>self._owner)._access.require_idle()
        with self._owner._operation():
            _check_passive(swmm_passive_exchange_enable_checkpoint(<SWMM_Engine><size_t>self._address()))

    def status(self):
        """Copy the native V1 status observation, preserving exact amount components."""
        cdef SWMM_PassiveStatusV1 value
        (<Solver>self._owner)._access.require_idle()
        with self._owner._operation():
            _check_passive(swmm_passive_exchange_status_v1(<SWMM_Engine><size_t>self._address(), sizeof(value), &value))
        return _status(value)

    def pool(self, int node):
        """Copy the native V1 pool observation, preserving exact amount components."""
        cdef SWMM_PassivePoolV1 value
        (<Solver>self._owner)._access.require_idle()
        with self._owner._operation():
            _check_passive(swmm_passive_exchange_pool_v1(<SWMM_Engine><size_t>self._address(), node, sizeof(value), &value))
        return _pool(value)

    def receipt(self, uint64_t receipt):
        """Copy the native V1 receipt observation, preserving exact amount components."""
        cdef SWMM_PassiveReceiptV1 value
        (<Solver>self._owner)._access.require_idle()
        with self._owner._operation():
            _check_passive(swmm_passive_exchange_receipt_v1(<SWMM_Engine><size_t>self._address(), receipt, sizeof(value), &value))
        return _receipt(value)

    def row(self, uint64_t receipt, uint64_t row, int remaining=0):
        """Copy the native V1 row observation, preserving exact amount components."""
        cdef SWMM_PassiveRowV1 value
        (<Solver>self._owner)._access.require_idle()
        with self._owner._operation():
            _check_passive(swmm_passive_exchange_row_v1(<SWMM_Engine><size_t>self._address(), receipt, remaining, row, sizeof(value), &value))
        return _row(value)

    def leg(self, uint64_t leg):
        """Copy the native V1 leg observation, preserving exact amount components."""
        cdef SWMM_PassiveLegV1 value
        (<Solver>self._owner)._access.require_idle()
        with self._owner._operation():
            _check_passive(swmm_passive_exchange_leg_v1(<SWMM_Engine><size_t>self._address(), leg, sizeof(value), &value))
        return _leg(value)

    def attempt(self):
        """Copy the native V1 attempt observation, preserving exact amount components."""
        cdef SWMM_PassiveLegV1 value
        (<Solver>self._owner)._access.require_idle()
        with self._owner._operation():
            _check_passive(swmm_passive_exchange_attempt_v1(<SWMM_Engine><size_t>self._address(), sizeof(value), &value))
        return _leg(value)

