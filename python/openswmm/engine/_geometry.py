"""
Cross-Section Geometry
======================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

Provides the :class:`CrossSection` dataclass, which wraps the raw
``(shape, geom1, geom2, geom3, geom4)`` tuple returned by
:meth:`Links.get_xsect` with human-readable field labels and a
``shape_name`` derived from the :class:`XSectShape` enum.

Example::

    from openswmm.engine import Solver, Links, CrossSection

    with Solver("model.inp", "model.rpt", "model.out") as s:
        links = Links(s)
        xs = links.get_xsect_info(0)
        print(xs.shape_name)          # "CIRCULAR"
        print(xs.geom_labels)         # {"diameter": 1.2}
"""

from __future__ import annotations

from dataclasses import dataclass

from ._enums import XSectShape

# ---------------------------------------------------------------------------
# Shape-name lookup
# ---------------------------------------------------------------------------

_XSECT_SHAPE_NAMES: dict[int, str] = {
    int(s): s.name for s in XSectShape
}

# ---------------------------------------------------------------------------
# Geometry parameter labels per shape
# ---------------------------------------------------------------------------
# Each tuple entry corresponds to geom1, geom2, geom3, geom4 in order.
# Entries shorter than 4 mean the trailing geoms are unused / zero.

_GEOM_LABELS: dict[int, tuple[str, ...]] = {
    XSectShape.CIRCULAR:          ("diameter",),
    XSectShape.FILLED_CIRCULAR:   ("diameter", "filled_depth"),
    XSectShape.RECT_CLOSED:       ("height", "width"),
    XSectShape.RECT_OPEN:         ("height", "width"),
    XSectShape.TRAPEZOIDAL:       ("height", "bottom_width", "side_slope"),
    XSectShape.TRIANGULAR:        ("height", "top_width"),
    XSectShape.PARABOLIC:         ("height", "top_width"),
    XSectShape.POWER:             ("height", "top_width", "exponent"),
    XSectShape.MODBASKETHANDLE:   ("height", "bottom_width", "top_radius"),
    XSectShape.EGGSHAPED:         ("height",),
    XSectShape.HORSESHOE:         ("height",),
    XSectShape.GOTHIC:            ("height",),
    XSectShape.CATENARY:          ("height",),
    XSectShape.SEMIELLIPTICAL:    ("height",),
    XSectShape.BASKETHANDLE:      ("height",),
    XSectShape.SEMICIRCULAR:      ("height",),
    XSectShape.IRREGULAR:         ("transect_index",),
    XSectShape.CUSTOM:            ("shape_curve_index",),
    XSectShape.FORCE_MAIN:        ("diameter", "roughness"),
}

# Shapes not in the enum (ellipse/arch variants that some engine builds expose)
_GEOM_LABELS_EXTRA: dict[int, tuple[str, ...]] = {
    # HORIZ_ELLIPSE / VERT_ELLIPSE / ARCH (integer codes beyond the enum)
    19: ("height", "width"),
    20: ("height", "width"),
    21: ("height", "width"),
}


def _resolve_geom_labels(shape: int) -> tuple[str, ...]:
    """Return the ordered label tuple for *shape*, falling back to generic names."""
    labels = _GEOM_LABELS.get(shape) or _GEOM_LABELS_EXTRA.get(shape)
    if labels is not None:
        return labels
    return ("geom1", "geom2", "geom3", "geom4")


# ---------------------------------------------------------------------------
# CrossSection dataclass
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class CrossSection:
    """Structured cross-section geometry returned by :meth:`Links.get_xsect_info`.

    All four ``geom`` values are always present; unused parameters are ``0.0``.
    Use :attr:`geom_labels` to get a ``{label: value}`` dict that filters out
    unused (zero) parameters and names each one meaningfully.

    @ivar shape: Integer cross-section shape code (see :class:`XSectShape`).
    @ivar shape_name: Human-readable name, e.g. ``"CIRCULAR"``.
    @ivar geom1: First geometry parameter (meaning depends on shape).
    @ivar geom2: Second geometry parameter (or 0.0 if unused).
    @ivar geom3: Third geometry parameter (or 0.0 if unused).
    @ivar geom4: Fourth geometry parameter (or 0.0 if unused).
    """

    shape: int
    shape_name: str
    geom1: float
    geom2: float
    geom3: float
    geom4: float

    @property
    def geom_labels(self) -> dict[str, float]:
        """Return ``{label: value}`` for the geometry parameters of this shape.

        Only parameters meaningful for this shape are included (trailing
        unused zeros are omitted).  Example for a 1.2 m diameter circular pipe::

            {"diameter": 1.2}

        @rtype: dict[str, float]
        """
        labels = _resolve_geom_labels(self.shape)
        values = (self.geom1, self.geom2, self.geom3, self.geom4)
        return {label: values[i] for i, label in enumerate(labels)}

    def __repr__(self) -> str:
        labels = self.geom_labels
        params = ", ".join(f"{k}={v}" for k, v in labels.items())
        return f"CrossSection({self.shape_name}, {params})"
