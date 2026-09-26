from __future__ import annotations
from dataclasses import dataclass
from typing import NamedTuple
from ._enums import TransportDispersionMode, TransportDomain, TransportClass, TransportState
from typing import TYPE_CHECKING
if TYPE_CHECKING:
    from ._solver import Solver
    from ._model import ModelBuilder

class ThreadInfo(NamedTuple):
    logical_cpus: int
    omp_max_threads: int
    omp_available: bool
    perf_cores: int
    kokkos_omp_threads: int
class EffectiveThreads(NamedTuple):
    global_threads: int
    dynamic_wave: int
    surface2d: int
@dataclass(frozen=True)
class TransportCell:
    state: TransportState
    count: int
    reason: str
class TransportRow(NamedTuple):
    element: str
    species: str
    is_timeseries: bool
    value: float
    timeseries: str
class ConduitDispersion(NamedTuple):
    link_index: int
    value: float
class Transport:
    def __init__(self, owner: Solver | ModelBuilder) -> None: ...
    @property
    def configured(self) -> bool: ...
    dispersion_mode: TransportDispersionMode
    dispersion_value: float
    target_dx: float
    @property
    def conduit_dispersion(self) -> tuple[ConduitDispersion, ...]: ...
    @property
    def boundaries(self) -> tuple[TransportRow, ...]: ...
    @property
    def sources(self) -> tuple[TransportRow, ...]: ...
def thread_info() -> ThreadInfo: ...
def effective_threads(owner: Solver, requested: int = ...) -> EffectiveThreads: ...
def transport_matrix(owner: Solver) -> dict[TransportDomain, dict[TransportClass, TransportCell]]: ...
def transport_domain_name(domain: TransportDomain) -> str: ...
def transport_class_name(species_class: TransportClass) -> str: ...
