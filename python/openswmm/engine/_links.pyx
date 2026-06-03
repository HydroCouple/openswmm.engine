"""
Link access (Pythonic v1 surface)
=================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

The :class:`Links` collection and :class:`Link` wrapper expose the
network's conveyance elements — conduits, pumps, orifices, weirs, and
outlets — with the same shape as :mod:`openswmm.engine._nodes`.

.. code-block:: python

    from openswmm.engine import Solver, LinkType, WeirType, XSectShape

    with Solver("model.inp") as s:
        c1 = s.links["C1"]
        print(c1.length, c1.roughness, c1.slope)

        # Cross-section as a property tuple (shape, g1..g4).
        c1.xsect = (XSectShape.CIRCULAR, 1.0, 0.0, 0.0, 0.0)

        # Bulk numpy access.
        flows = s.links.flows                   # np.ndarray
        s.links.flows = flows * 0.95

        # Topology — from/to return Node wrappers.
        print(c1.from_node.id, c1.to_node.id)

        # Type-specific sub-views (raise AttributeError on wrong type).
        s.links["P1"].pump.curve = 0
        s.links["W1"].weir.type = WeirType.TRANSVERSE
"""

# cython: language_level=3

import numpy as np
cimport numpy as np

from ._common cimport *
from ._enums import LinkType, OrificeType, OutletRatingType, WeirType, XSectShape
from ._exceptions import StaleObjectError


# =============================================================================
# Helpers
# =============================================================================

cdef inline SWMM_Engine _h(solver):
    return <SWMM_Engine><size_t>solver.handle


cdef inline int _resolve_link(solver, object key) except -1:
    return _resolve_index(_h(solver), key, swmm_link_index, swmm_link_count, "Link")


cdef inline void _check_fresh(link) except *:
    if link._gen != link._solver.generation:
        raise StaleObjectError(
            f"Link wrapper (id={link._captured_id!r}, index={link._index}) is stale; "
            "look it up again from solver.links."
        )


cdef int _resolve_pollutant(solver, key) except -1:
    return _resolve_index(
        _h(solver), key, swmm_pollutant_index, swmm_pollutant_count, "Pollutant")


# =============================================================================
# Sub-views
# =============================================================================

cdef class LinkStatsView:
    """Per-link summary statistics. Pump-specific entries raise
    :class:`BadParamError` on non-pump links."""
    cdef object _link

    def __init__(self, link):
        self._link = link

    @property
    def max_flow(self) -> float:
        _check_fresh(self._link)
        cdef double v = 0.0
        _check(swmm_link_get_stat_max_flow(_h(self._link._solver), self._link._index, &v))
        return v

    @property
    def max_velocity(self) -> float:
        _check_fresh(self._link)
        cdef double v = 0.0
        _check(swmm_link_get_stat_max_velocity(_h(self._link._solver), self._link._index, &v))
        return v

    @property
    def max_filling(self) -> float:
        _check_fresh(self._link)
        cdef double v = 0.0
        _check(swmm_link_get_stat_max_filling(_h(self._link._solver), self._link._index, &v))
        return v

    @property
    def vol_flow(self) -> float:
        _check_fresh(self._link)
        cdef double v = 0.0
        _check(swmm_link_get_stat_vol_flow(_h(self._link._solver), self._link._index, &v))
        return v

    @property
    def surcharge_time(self) -> float:
        _check_fresh(self._link)
        cdef double v = 0.0
        _check(swmm_link_get_stat_surcharge_time(_h(self._link._solver), self._link._index, &v))
        return v

    @property
    def pump_cycles(self) -> int:
        """Cycles for a pump link. Raises :class:`BadParamError` on non-pumps."""
        _check_fresh(self._link)
        cdef int v = 0
        _check(swmm_link_get_stat_pump_cycles(_h(self._link._solver), self._link._index, &v))
        return v

    @property
    def pump_on_time(self) -> float:
        _check_fresh(self._link)
        cdef double v = 0.0
        _check(swmm_link_get_stat_pump_on_time(_h(self._link._solver), self._link._index, &v))
        return v

    @property
    def pump_volume(self) -> float:
        _check_fresh(self._link)
        cdef double v = 0.0
        _check(swmm_link_get_stat_pump_volume(_h(self._link._solver), self._link._index, &v))
        return v

    def __repr__(self) -> str:
        return f"<LinkStatsView for {self._link!r}>"


cdef class XSection:
    """``link.xsect`` — read/write view over the cross-section parameters.

    Returning ``link.xsect`` gives a snapshot ``(shape, g1, g2, g3, g4)``
    tuple. Assigning ``link.xsect = (shape, g1, g2, g3, g4)`` writes all
    five fields through ``swmm_link_set_xsect``.
    """
    cdef object _link

    def __init__(self, link):
        self._link = link

    def _read(self):
        _check_fresh(self._link)
        cdef int shape = 0
        cdef double g1 = 0.0, g2 = 0.0, g3 = 0.0, g4 = 0.0
        _check(swmm_link_get_xsect(
            _h(self._link._solver), self._link._index,
            &shape, &g1, &g2, &g3, &g4))
        return (shape, g1, g2, g3, g4)

    @property
    def shape(self):
        return XSectShape(self._read()[0])

    @property
    def g1(self) -> float:
        return self._read()[1]

    @property
    def g2(self) -> float:
        return self._read()[2]

    @property
    def g3(self) -> float:
        return self._read()[3]

    @property
    def g4(self) -> float:
        return self._read()[4]

    def as_tuple(self):
        s, g1, g2, g3, g4 = self._read()
        return (XSectShape(s), g1, g2, g3, g4)

    def __iter__(self):
        return iter(self.as_tuple())

    def __repr__(self) -> str:
        try:
            shape, g1, g2, g3, g4 = self.as_tuple()
            return f"<XSection shape={shape.name} g=({g1}, {g2}, {g3}, {g4})>"
        except Exception:
            return f"<XSection (stale or closed)>"


cdef class PumpView:
    """``link.pump`` — pump-only configuration."""
    cdef object _link

    def __init__(self, link):
        self._link = link

    @property
    def curve(self) -> int:
        _check_fresh(self._link)
        cdef int v = 0
        _check(swmm_link_get_pump_curve(_h(self._link._solver), self._link._index, &v))
        return v

    @curve.setter
    def curve(self, int curve_idx) -> None:
        _check_fresh(self._link)
        _check(swmm_link_set_pump_curve(_h(self._link._solver), self._link._index, curve_idx))

    @property
    def init_state(self) -> bool:
        _check_fresh(self._link)
        cdef int v = 0
        _check(swmm_link_get_pump_init_state(_h(self._link._solver), self._link._index, &v))
        return v != 0

    @init_state.setter
    def init_state(self, bint on) -> None:
        _check_fresh(self._link)
        _check(swmm_link_set_pump_init_state(
            _h(self._link._solver), self._link._index, 1 if on else 0))

    @property
    def startup_depth(self) -> float:
        _check_fresh(self._link)
        cdef double v = 0.0
        _check(swmm_link_get_pump_startup_depth(_h(self._link._solver), self._link._index, &v))
        return v

    @startup_depth.setter
    def startup_depth(self, double depth) -> None:
        _check_fresh(self._link)
        _check(swmm_link_set_pump_startup_depth(_h(self._link._solver), self._link._index, depth))

    @property
    def shutoff_depth(self) -> float:
        _check_fresh(self._link)
        cdef double v = 0.0
        _check(swmm_link_get_pump_shutoff_depth(_h(self._link._solver), self._link._index, &v))
        return v

    @shutoff_depth.setter
    def shutoff_depth(self, double depth) -> None:
        _check_fresh(self._link)
        _check(swmm_link_set_pump_shutoff_depth(_h(self._link._solver), self._link._index, depth))

    def __repr__(self) -> str:
        return f"<PumpView for {self._link!r}>"


cdef class WeirView:
    """``link.weir`` — weir-only configuration."""
    cdef object _link

    def __init__(self, link):
        self._link = link

    @property
    def type(self):
        _check_fresh(self._link)
        cdef int v = 0
        _check(swmm_link_get_weir_type(_h(self._link._solver), self._link._index, &v))
        return WeirType(v)

    @type.setter
    def type(self, value) -> None:
        _check_fresh(self._link)
        _check(swmm_link_set_weir_type(_h(self._link._solver), self._link._index, int(value)))

    @property
    def crest_height(self) -> float:
        _check_fresh(self._link)
        cdef double v = 0.0
        _check(swmm_link_get_crest_height(_h(self._link._solver), self._link._index, &v))
        return v

    @crest_height.setter
    def crest_height(self, double h) -> None:
        _check_fresh(self._link)
        _check(swmm_link_set_crest_height(_h(self._link._solver), self._link._index, h))

    @property
    def discharge_coeff(self) -> float:
        _check_fresh(self._link)
        cdef double v = 0.0
        _check(swmm_link_get_discharge_coeff(_h(self._link._solver), self._link._index, &v))
        return v

    @discharge_coeff.setter
    def discharge_coeff(self, double cd) -> None:
        _check_fresh(self._link)
        _check(swmm_link_set_discharge_coeff(_h(self._link._solver), self._link._index, cd))

    @property
    def end_contractions(self) -> float:
        _check_fresh(self._link)
        cdef double v = 0.0
        _check(swmm_link_get_end_contractions(_h(self._link._solver), self._link._index, &v))
        return v

    @end_contractions.setter
    def end_contractions(self, double n) -> None:
        _check_fresh(self._link)
        _check(swmm_link_set_end_contractions(_h(self._link._solver), self._link._index, n))

    def __repr__(self) -> str:
        return f"<WeirView for {self._link!r}>"


cdef class OrificeView:
    """``link.orifice`` — orifice-only configuration."""
    cdef object _link

    def __init__(self, link):
        self._link = link

    @property
    def type(self):
        _check_fresh(self._link)
        cdef int v = 0
        _check(swmm_link_get_orifice_type(_h(self._link._solver), self._link._index, &v))
        return OrificeType(v)

    @type.setter
    def type(self, value) -> None:
        _check_fresh(self._link)
        _check(swmm_link_set_orifice_type(_h(self._link._solver), self._link._index, int(value)))

    @property
    def open_close_rate(self) -> float:
        _check_fresh(self._link)
        cdef double v = 0.0
        _check(swmm_link_get_orifice_open_close_rate(_h(self._link._solver), self._link._index, &v))
        return v

    @open_close_rate.setter
    def open_close_rate(self, double rate) -> None:
        _check_fresh(self._link)
        _check(swmm_link_set_orifice_open_close_rate(_h(self._link._solver), self._link._index, rate))

    def __repr__(self) -> str:
        return f"<OrificeView for {self._link!r}>"


cdef class OutletView:
    """``link.outlet`` — outlet-only configuration."""
    cdef object _link

    def __init__(self, link):
        self._link = link

    @property
    def rating_type(self):
        _check_fresh(self._link)
        cdef int v = 0
        _check(swmm_link_get_outlet_rating_type(_h(self._link._solver), self._link._index, &v))
        return OutletRatingType(v)

    @rating_type.setter
    def rating_type(self, value) -> None:
        _check_fresh(self._link)
        _check(swmm_link_set_outlet_rating_type(
            _h(self._link._solver), self._link._index, int(value)))

    @property
    def expon(self) -> float:
        _check_fresh(self._link)
        cdef double v = 0.0
        _check(swmm_link_get_outlet_expon(_h(self._link._solver), self._link._index, &v))
        return v

    @expon.setter
    def expon(self, double value) -> None:
        _check_fresh(self._link)
        _check(swmm_link_set_outlet_expon(_h(self._link._solver), self._link._index, value))

    def __repr__(self) -> str:
        return f"<OutletView for {self._link!r}>"


# =============================================================================
# Link wrapper
# =============================================================================

cdef class Link:
    """A single link, addressed by index in the parent :class:`Links`
    collection. Same staleness model as :class:`Node`."""

    cdef readonly object _solver
    cdef readonly int _index
    cdef readonly long long _gen
    cdef readonly str _captured_id
    cdef object _stats
    cdef object _xsect
    cdef object _pump
    cdef object _weir
    cdef object _orifice
    cdef object _outlet

    def __init__(self, solver, int index):
        self._solver = solver
        self._index = index
        self._gen = solver.generation
        cdef const char* raw = swmm_link_id(_h(solver), index)
        self._captured_id = raw.decode('utf-8') if raw != NULL else ""
        self._stats = None
        self._xsect = None
        self._pump = None
        self._weir = None
        self._orifice = None
        self._outlet = None

    # ---- Identity ---------------------------------------------------

    @property
    def id(self) -> str:
        _check_fresh(self)
        cdef const char* raw = swmm_link_id(_h(self._solver), self._index)
        return raw.decode('utf-8') if raw != NULL else ""

    @property
    def tag(self) -> str:
        """The link's free-form tag string (from the INP C{[TAGS]} section).

        Empty string when the link has no tag. Assigning C{None} or C{""}
        clears it. The tag is keyed by index and persists across L{rename}.

        @rtype: str
        """
        _check_fresh(self)
        cdef char buf[256]
        _check(swmm_link_get_tag(_h(self._solver), self._index, buf, 256))
        return buf.decode('utf-8')

    @tag.setter
    def tag(self, value) -> None:
        _check_fresh(self)
        cdef bytes b = (value or "").encode('utf-8')
        _check(swmm_link_set_tag(_h(self._solver), self._index, b))

    @property
    def index(self) -> int:
        _check_fresh(self)
        return self._index

    @property
    def type(self):
        _check_fresh(self)
        cdef int v = 0
        _check(swmm_link_get_type(_h(self._solver), self._index, &v))
        return LinkType(v)

    @property
    def solver(self):
        return self._solver

    # ---- Topology ---------------------------------------------------

    @property
    def from_node(self):
        """Upstream :class:`Node` wrapper."""
        _check_fresh(self)
        cdef int v = 0
        _check(swmm_link_get_from_node(_h(self._solver), self._index, &v))
        from ._nodes import Node
        return Node(self._solver, v)

    @property
    def to_node(self):
        """Downstream :class:`Node` wrapper."""
        _check_fresh(self)
        cdef int v = 0
        _check(swmm_link_get_to_node(_h(self._solver), self._index, &v))
        from ._nodes import Node
        return Node(self._solver, v)

    def set_nodes(self, from_node, to_node) -> None:
        """Reconnect this link. Accepts ``int`` indices, ``str`` ids, or
        :class:`Node` wrappers."""
        _check_fresh(self)
        from ._nodes import Node
        cdef int f, t
        if isinstance(from_node, Node):
            f = from_node._index
        else:
            f = _resolve_index(
                _h(self._solver), from_node,
                swmm_node_index, swmm_node_count, "Node")
        if isinstance(to_node, Node):
            t = to_node._index
        else:
            t = _resolve_index(
                _h(self._solver), to_node,
                swmm_node_index, swmm_node_count, "Node")
        _check(swmm_link_set_nodes(_h(self._solver), self._index, f, t))

    # ---- Geometry ---------------------------------------------------

    @property
    def length(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_link_get_length(_h(self._solver), self._index, &v))
        return v

    @length.setter
    def length(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_link_set_length(_h(self._solver), self._index, value))

    @property
    def roughness(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_link_get_roughness(_h(self._solver), self._index, &v))
        return v

    @roughness.setter
    def roughness(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_link_set_roughness(_h(self._solver), self._index, value))

    @property
    def slope(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_link_get_slope(_h(self._solver), self._index, &v))
        return v

    @property
    def offset_up(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_link_get_offset_up(_h(self._solver), self._index, &v))
        return v

    @offset_up.setter
    def offset_up(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_link_set_offset_up(_h(self._solver), self._index, value))

    @property
    def offset_dn(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_link_get_offset_dn(_h(self._solver), self._index, &v))
        return v

    @offset_dn.setter
    def offset_dn(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_link_set_offset_dn(_h(self._solver), self._index, value))

    @property
    def initial_flow(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_link_get_initial_flow(_h(self._solver), self._index, &v))
        return v

    @initial_flow.setter
    def initial_flow(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_link_set_initial_flow(_h(self._solver), self._index, value))

    @property
    def max_flow(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_link_get_max_flow(_h(self._solver), self._index, &v))
        return v

    @max_flow.setter
    def max_flow(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_link_set_max_flow(_h(self._solver), self._index, value))

    # ---- Cross-section ---------------------------------------------

    @property
    def xsect(self) -> XSection:
        if self._xsect is None:
            self._xsect = XSection(self)
        return self._xsect

    @xsect.setter
    def xsect(self, value) -> None:
        _check_fresh(self)
        # Accept (shape, g1, g2, g3, g4) tuple, or an XSection instance.
        if isinstance(value, XSection):
            shape, g1, g2, g3, g4 = value.as_tuple()
        else:
            shape, g1, g2, g3, g4 = value
        _check(swmm_link_set_xsect(
            _h(self._solver), self._index, int(shape), g1, g2, g3, g4))

    # ---- Hydraulic state -------------------------------------------

    @property
    def flow(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_link_get_flow(_h(self._solver), self._index, &v))
        return v

    @flow.setter
    def flow(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_link_set_flow(_h(self._solver), self._index, value))

    @property
    def depth(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_link_get_depth(_h(self._solver), self._index, &v))
        return v

    @property
    def velocity(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_link_get_velocity(_h(self._solver), self._index, &v))
        return v

    @property
    def capacity(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_link_get_capacity(_h(self._solver), self._index, &v))
        return v

    @property
    def volume(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_link_get_volume(_h(self._solver), self._index, &v))
        return v

    @property
    def hyd_power(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_link_get_hyd_power(_h(self._solver), self._index, &v))
        return v

    # ---- Control settings -----------------------------------------

    @property
    def control_setting(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_link_get_control_setting(_h(self._solver), self._index, &v))
        return v

    @control_setting.setter
    def control_setting(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_link_set_control_setting(_h(self._solver), self._index, value))

    @property
    def target_setting(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_link_get_target_setting(_h(self._solver), self._index, &v))
        return v

    @target_setting.setter
    def target_setting(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_link_set_target_setting(_h(self._solver), self._index, value))

    @property
    def closed(self) -> bool:
        _check_fresh(self)
        cdef int v = 0
        _check(swmm_link_get_closed(_h(self._solver), self._index, &v))
        return v != 0

    @closed.setter
    def closed(self, bint value) -> None:
        _check_fresh(self)
        _check(swmm_link_set_closed(_h(self._solver), self._index, 1 if value else 0))

    # ---- Common conduit knobs --------------------------------------

    @property
    def loss_coeff(self) -> tuple:
        """``(inlet, outlet, avg)`` loss coefficients."""
        _check_fresh(self)
        cdef double i = 0.0, o = 0.0, a = 0.0
        _check(swmm_link_get_loss_coeff(_h(self._solver), self._index, &i, &o, &a))
        return (i, o, a)

    @loss_coeff.setter
    def loss_coeff(self, value) -> None:
        _check_fresh(self)
        inlet, outlet, avg = value
        _check(swmm_link_set_loss_coeff(
            _h(self._solver), self._index, inlet, outlet, avg))

    @property
    def flap_gate(self) -> bool:
        _check_fresh(self)
        cdef int v = 0
        _check(swmm_link_get_flap_gate(_h(self._solver), self._index, &v))
        return v != 0

    @flap_gate.setter
    def flap_gate(self, bint value) -> None:
        _check_fresh(self)
        _check(swmm_link_set_flap_gate(
            _h(self._solver), self._index, 1 if value else 0))

    @property
    def seep_rate(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_link_get_seep_rate(_h(self._solver), self._index, &v))
        return v

    @seep_rate.setter
    def seep_rate(self, double rate) -> None:
        _check_fresh(self)
        _check(swmm_link_set_seep_rate(_h(self._solver), self._index, rate))

    @property
    def culvert_code(self) -> int:
        _check_fresh(self)
        cdef int v = 0
        _check(swmm_link_get_culvert_code(_h(self._solver), self._index, &v))
        return v

    @culvert_code.setter
    def culvert_code(self, int code) -> None:
        _check_fresh(self)
        _check(swmm_link_set_culvert_code(_h(self._solver), self._index, code))

    @property
    def barrels(self) -> int:
        _check_fresh(self)
        cdef int v = 0
        _check(swmm_link_get_barrels(_h(self._solver), self._index, &v))
        return v

    @barrels.setter
    def barrels(self, int n) -> None:
        _check_fresh(self)
        _check(swmm_link_set_barrels(_h(self._solver), self._index, n))

    # ---- Quality ---------------------------------------------------

    def quality(self, pollutant) -> float:
        _check_fresh(self)
        cdef int p_idx = _resolve_pollutant(self._solver, pollutant)
        cdef double v = 0.0
        _check(swmm_link_get_quality(_h(self._solver), self._index, p_idx, &v))
        return v

    # ---- Sub-views -------------------------------------------------

    @property
    def stats(self) -> LinkStatsView:
        if self._stats is None:
            self._stats = LinkStatsView(self)
        return self._stats

    @property
    def pump(self) -> PumpView:
        if self.type != LinkType.PUMP:
            raise AttributeError(
                f"link {self.id!r} is a {self.type.name}, not PUMP; "
                "the .pump sub-view is only valid for pump links")
        if self._pump is None:
            self._pump = PumpView(self)
        return self._pump

    @property
    def weir(self) -> WeirView:
        if self.type != LinkType.WEIR:
            raise AttributeError(
                f"link {self.id!r} is a {self.type.name}, not WEIR; "
                "the .weir sub-view is only valid for weir links")
        if self._weir is None:
            self._weir = WeirView(self)
        return self._weir

    @property
    def orifice(self) -> OrificeView:
        if self.type != LinkType.ORIFICE:
            raise AttributeError(
                f"link {self.id!r} is a {self.type.name}, not ORIFICE; "
                "the .orifice sub-view is only valid for orifice links")
        if self._orifice is None:
            self._orifice = OrificeView(self)
        return self._orifice

    @property
    def outlet(self) -> OutletView:
        if self.type != LinkType.OUTLET:
            raise AttributeError(
                f"link {self.id!r} is a {self.type.name}, not OUTLET; "
                "the .outlet sub-view is only valid for outlet links")
        if self._outlet is None:
            self._outlet = OutletView(self)
        return self._outlet

    # ---- Equality / repr ------------------------------------------

    def __eq__(self, other):
        if not isinstance(other, Link):
            return NotImplemented
        return (self._solver is other._solver
                and self._index == other._index)

    def __hash__(self):
        return hash((id(self._solver), self._index))

    def __repr__(self) -> str:
        try:
            return f"<Link id={self._captured_id!r} index={self._index}>"
        except Exception:
            return f"<Link index={self._index} (stale or closed)>"


# =============================================================================
# Links collection
# =============================================================================

cdef class Links:
    """Indexable, iterable collection of :class:`Link` wrappers."""

    cdef object _solver

    def __init__(self, solver):
        self._solver = solver

    # ---- Container protocol ----------------------------------------

    def __len__(self) -> int:
        return swmm_link_count(_h(self._solver))

    def __iter__(self):
        cdef int n = swmm_link_count(_h(self._solver))
        for i in range(n):
            yield Link(self._solver, i)

    def __getitem__(self, key) -> Link:
        cdef int i = _resolve_link(self._solver, key)
        return Link(self._solver, i)

    def __contains__(self, key) -> bool:
        try:
            _resolve_link(self._solver, key)
            return True
        except (KeyError, IndexError, TypeError):
            return False

    # ---- Identity lookups -----------------------------------------

    def get_index(self, str link_id) -> int:
        cdef bytes b = link_id.encode('utf-8')
        cdef int i = swmm_link_index(_h(self._solver), b)
        if i < 0:
            raise KeyError(link_id)
        return i

    def get_id(self, int idx) -> str:
        if not (0 <= idx < len(self)):
            raise IndexError(idx)
        cdef const char* raw = swmm_link_id(_h(self._solver), idx)
        return raw.decode('utf-8') if raw != NULL else ""

    # ---- Editing (bumps generation) -------------------------------

    def add(self, str link_id, link_type) -> Link:
        cdef bytes b = link_id.encode('utf-8')
        _check(swmm_link_add(_h(self._solver), b, int(link_type)))
        self._solver._bump_generation()
        cdef int new_idx = swmm_link_index(_h(self._solver), b)
        return Link(self._solver, new_idx)

    def pop_last(self, str link_id) -> None:
        cdef bytes b = link_id.encode('utf-8')
        _check(swmm_link_pop_last(_h(self._solver), b))
        self._solver._bump_generation()

    def rename(self, key, str new_id) -> None:
        cdef int i = _resolve_link(self._solver, key)
        cdef bytes b = new_id.encode('utf-8')
        _check(swmm_link_rename(_h(self._solver), i, b))
        self._solver._bump_generation()

    # ---- Bulk numpy properties ------------------------------------

    @property
    def flows(self):
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_link_get_flows_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @flows.setter
    def flows(self, values) -> None:
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] arr = np.ascontiguousarray(values, dtype=np.float64)
        if arr.shape[0] != n:
            raise ValueError(f"flows array length {arr.shape[0]} != link count {n}")
        cdef const double* p = <const double*>arr.data
        cdef int err
        with nogil:
            err = swmm_link_set_flows_bulk(h, p, n)
        _check(err)

    @property
    def depths(self):
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_link_get_depths_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def velocities(self):
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_link_get_velocities_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def capacities(self):
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_link_get_capacities_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def volumes(self):
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_link_get_volumes_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def control_settings(self):
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_link_get_control_settings_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def target_settings(self):
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_link_get_target_settings_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def hyd_powers(self):
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_link_get_hyd_powers_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    def qualities(self, pollutant):
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_link_count(h)
        cdef int p_idx = _resolve_pollutant(self._solver, pollutant)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_link_get_quality_bulk(h, p_idx, <double*>buf.data, n)
        _check(err)
        return buf

    def pump_stats(self):
        """Per-link pump statistics as a 3-tuple of arrays
        ``(cycles, on_time, volume)``. Non-pump links have zeros."""
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[int, ndim=1] cyc = np.zeros(n, dtype=np.int32)
        cdef np.ndarray[double, ndim=1] ont = np.zeros(n, dtype=np.float64)
        cdef np.ndarray[double, ndim=1] vol = np.zeros(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_link_get_pump_stats_bulk(
                h, <int*>cyc.data, <double*>ont.data, <double*>vol.data, n)
        _check(err)
        return (cyc, ont, vol)

    @property
    def ids(self):
        return np.asarray(self._ids_list(), dtype=object)

    def _ids_list(self, int stride=64):
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[char, ndim=1, mode="c"] buf = np.zeros(
            n * stride, dtype=np.int8)
        cdef int err
        with nogil:
            err = swmm_link_get_ids_bulk(h, <char*>buf.data, stride, n)
        _check(err)
        raw = bytes(buf)
        out = []
        for i in range(n):
            slot = raw[i * stride:(i + 1) * stride]
            nul = slot.find(b"\x00")
            if nul >= 0:
                slot = slot[:nul]
            out.append(slot.decode("utf-8"))
        return out

    def __repr__(self) -> str:
        try:
            return f"<Links n={len(self)}>"
        except Exception:
            return "<Links (engine closed)>"
