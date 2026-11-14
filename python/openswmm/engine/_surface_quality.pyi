from __future__ import annotations
from dataclasses import dataclass
from collections.abc import Mapping
from typing import Any
import numpy as np
from numpy.typing import NDArray
from ._enums import CellScope

@dataclass(frozen=True)
class SurfaceCoverage:
    """A whole land-use coverage set at one scope; pairs carry percentages."""
    scope: CellScope
    tag: str
    cell: int
    landuses: tuple[tuple[str, float], ...]

@dataclass(frozen=True)
class SurfaceLoading:
    """Initial species buildup in project mass per acre/hectare."""
    scope: CellScope
    tag: str
    cell: int
    species: str
    value: float

@dataclass(frozen=True)
class SurfaceCurbLength:
    """Authored curb length in project length units."""
    scope: CellScope
    tag: str
    cell: int
    length: float

class SurfaceQuality:
    def __init__(self, owner: Any) -> None: ...
    @property
    def coverages(self) -> tuple[SurfaceCoverage, ...]: ...
    def set_coverage(self, scope: CellScope, landuses: Mapping[str, float], *, tag: str = ..., cell: int = ...) -> None: ...
    def remove_coverage(self, index: int) -> None: ...
    @property
    def loadings(self) -> tuple[SurfaceLoading, ...]: ...
    def set_loading(self, scope: CellScope, species: str, value: float, *, tag: str = ..., cell: int = ...) -> None: ...
    def remove_loading(self, index: int) -> None: ...
    @property
    def curb_lengths(self) -> tuple[SurfaceCurbLength, ...]: ...
    def set_curb_length(self, scope: CellScope, length: float, *, tag: str = ..., cell: int = ...) -> None: ...
    def remove_curb_length(self, index: int) -> None: ...
    def buildup(self, species: str) -> NDArray[np.float64]: ...
