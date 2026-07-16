"""Type stubs for openswmm.engine._xsect."""

from typing import Any, Literal, Sequence, Union, overload

import numpy as np
from numpy.typing import NDArray

from ._enums import XSectShape

_Units = Literal["US", "SI"]
_Scalar = float
_Array = NDArray[np.float64]
_Value = Union[float, Sequence[float], _Array]


class XSectionGeometry:
    shape: XSectShape

    def __init__(
        self,
        shape: XSectShape | int,
        geom1: float,
        geom2: float = ...,
        geom3: float = ...,
        geom4: float = ...,
        *,
        units: _Units,
    ) -> None: ...

    @classmethod
    def from_transect(
        cls,
        stations: _Value,
        elevations: _Value,
        *,
        left_bank: float,
        right_bank: float,
        n_channel: float,
        n_left: float = ...,
        n_right: float = ...,
        length_factor: float = ...,
        units: _Units,
    ) -> "XSectionGeometry": ...

    @classmethod
    def from_curve(
        cls,
        full_depth: float,
        curve_depths: _Value,
        curve_widths: _Value,
        *,
        units: _Units,
    ) -> "XSectionGeometry": ...

    @classmethod
    def from_street(
        cls,
        width: float,
        curb_height: float,
        slope: float,
        roughness: float,
        *,
        gutter_depression: float = ...,
        gutter_width: float = ...,
        sides: int = ...,
        back_width: float = ...,
        back_slope: float = ...,
        back_roughness: float = ...,
        units: _Units,
    ) -> "XSectionGeometry": ...

    @classmethod
    def from_link(cls, link: Any) -> "XSectionGeometry": ...

    @overload
    def area(self, depth: float) -> float: ...
    @overload
    def area(self, depth: Sequence[float] | _Array) -> _Array: ...

    @overload
    def width(self, depth: float) -> float: ...
    @overload
    def width(self, depth: Sequence[float] | _Array) -> _Array: ...

    @overload
    def hyd_radius(self, depth: float) -> float: ...
    @overload
    def hyd_radius(self, depth: Sequence[float] | _Array) -> _Array: ...

    @overload
    def depth_from_area(self, area: float) -> float: ...
    @overload
    def depth_from_area(self, area: Sequence[float] | _Array) -> _Array: ...

    @overload
    def hyd_radius_from_area(self, area: float) -> float: ...
    @overload
    def hyd_radius_from_area(self, area: Sequence[float] | _Array) -> _Array: ...

    @overload
    def section_factor(self, area: float) -> float: ...
    @overload
    def section_factor(self, area: Sequence[float] | _Array) -> _Array: ...

    @overload
    def area_from_section_factor(self, sf: float) -> float: ...
    @overload
    def area_from_section_factor(self, sf: Sequence[float] | _Array) -> _Array: ...

    @overload
    def dsda(self, area: float) -> float: ...
    @overload
    def dsda(self, area: Sequence[float] | _Array) -> _Array: ...

    @overload
    def critical_depth(self, flow: float) -> float: ...
    @overload
    def critical_depth(self, flow: Sequence[float] | _Array) -> _Array: ...

    @property
    def units(self) -> _Units: ...
    @property
    def flow_units(self) -> str: ...
    @property
    def full_depth(self) -> float: ...
    @property
    def full_area(self) -> float: ...
    @property
    def full_hyd_radius(self) -> float: ...
    @property
    def max_width(self) -> float: ...
    @property
    def full_section_factor(self) -> float: ...
    @property
    def max_area(self) -> float: ...
    @property
    def is_open(self) -> bool: ...


def shape_name(shape: XSectShape | int) -> str: ...
