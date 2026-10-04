# SPDX-License-Identifier: Apache-2.0
"""Ordered storage-node LID layers, in the model's rainfall depth/rate units."""
from dataclasses import dataclass
from enum import IntEnum


class LidNodeLayerKind(IntEnum):
    SURFACE = 0
    MEDIA = 1
    AGGREGATE = 2
    BOTTOM = 3


@dataclass(frozen=True)
class LidNodeLayer:
    """One authored layer. Repeat MEDIA/AGGREGATE freely, in top-to-bottom order.

    ``params`` are: SURFACE (thickness, vegetation fraction), MEDIA
    (thickness, porosity, field capacity, wilting point, Ksat, Kslope, suction),
    AGGREGATE (thickness, porosity, Ksat), BOTTOM (seepage, clog factor).
    Thickness/suction use in or mm; conductivity/seepage use in/hr or mm/hr.
    BOTTOM is a boundary and does not consume a layer number for outlet anchors.
    """
    kind: LidNodeLayerKind
    params: tuple[float, ...]

    def __post_init__(self) -> None:
        kind = LidNodeLayerKind(self.kind)
        values = tuple(float(v) for v in self.params)
        if len(values) != (2, 7, 3, 2)[kind]:
            raise ValueError(f"{kind.name} requires {(2, 7, 3, 2)[kind]} parameters")
        object.__setattr__(self, "kind", kind)
        object.__setattr__(self, "params", values)


@dataclass(frozen=True)
class LidLayerTreatment:
    """Treatment on a one-based physical layer; removal %, decay per day.

    The optional R= or C= expression acts after fixed removal on outgoing
    water. Decay acts on retained pollutant mass once per routing step.
    """
    layer: int
    pollutant: str
    removal_percent: float = 0.0
    decay_per_day: float = 0.0
    expression: str = ""
