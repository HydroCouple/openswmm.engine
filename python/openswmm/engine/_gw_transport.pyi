from __future__ import annotations
from dataclasses import dataclass
from collections.abc import MutableMapping
from typing import Any
from os import PathLike
from ._enums import CellScope, GroundwaterTransportZone

@dataclass(frozen=True)
class GroundwaterParameters:
    """Immutable authored row; replace fields and submit it with the matching setter."""
    scope: CellScope = CellScope.GLOBAL
    cell: int = -1
    rho_s: float = 2650.0
    c_s: float = 880.0
    lambda_s: float = 2.0
    a_s: float = 0.0
    alpha_L: float = 1.0
    alpha_T: float = 0.1
    D_m: float = 1e-9
    D_v: float = 1e-9
    geo_flux: float = 0.065
    tag: str = ''


@dataclass(frozen=True)
class GroundwaterSorption:
    """Immutable authored row; replace fields and submit it with the matching setter."""
    scope: CellScope = CellScope.GLOBAL
    tag: str = ''
    cell: int = -1
    species: str = ''
    kd: float = 0.0
    decay: float = -1.0


@dataclass(frozen=True)
class GroundwaterInitialQuality:
    """Immutable authored row; replace fields and submit it with the matching setter."""
    scope: CellScope = CellScope.GLOBAL
    tag: str = ''
    cell: int = -1
    zone: GroundwaterTransportZone = GroundwaterTransportZone.SAT
    layer: int = -1
    species: str = ''
    value: float = 0.0


@dataclass(frozen=True)
class GroundwaterBoundary:
    """Immutable authored row; replace fields and submit it with the matching setter."""
    cell: int = 0
    edge: int = 0
    species: str = ''
    kind: str = 'CONC'
    value: float = 0.0
    timeseries: str = ''


@dataclass(frozen=True)
class GroundwaterSource:
    """Immutable authored row; replace fields and submit it with the matching setter."""
    name: str = ''
    scope: CellScope = CellScope.CELL
    tag: str = ''
    cell: int = 0
    flow: float = 0.0
    flow_timeseries: str = ''


@dataclass(frozen=True)
class GroundwaterSourceTerm:
    """Immutable authored row; replace fields and submit it with the matching setter."""
    species: str = ''
    kind: str = 'CONC'
    value: float = 0.0
    timeseries: str = ''


class GroundwaterTransport:
    def __init__(self, owner: Any) -> None: ...
    @property
    def options(self) -> MutableMapping[str, str]: ...
    @property
    def authored(self) -> bool: ...
    @property
    def parameters(self) -> tuple[GroundwaterParameters, ...]: ...
    def set_parameters(self, row: GroundwaterParameters) -> None: ...
    def remove_parameters(self, index: int) -> None: ...
    @property
    def sorption(self) -> tuple[GroundwaterSorption, ...]: ...
    def set_sorption(self, row: GroundwaterSorption) -> None: ...
    def remove_sorption(self, index: int) -> None: ...
    @property
    def initial_quality(self) -> tuple[GroundwaterInitialQuality, ...]: ...
    def set_initial_quality(self, row: GroundwaterInitialQuality) -> None: ...
    def remove_initial_quality(self, index: int) -> None: ...
    @property
    def boundaries(self) -> tuple[GroundwaterBoundary, ...]: ...
    def set_boundary(self, row: GroundwaterBoundary) -> None: ...
    def remove_boundary(self, index: int) -> None: ...
    @property
    def sources(self) -> tuple[GroundwaterSource, ...]: ...
    def set_source(self, row: GroundwaterSource) -> None: ...
    def remove_source(self, index: int) -> None: ...
    def source_species(self, source_index: int) -> tuple[GroundwaterSourceTerm, ...]: ...
    def set_source_species(self, source_index: int, row: GroundwaterSourceTerm) -> None: ...
    def remove_source_species(self, source_index: int, index: int) -> None: ...
    @property
    def initial_quality_file(self) -> str: ...
    @initial_quality_file.setter
    def initial_quality_file(self, path: str | PathLike[str] | None) -> None: ...
