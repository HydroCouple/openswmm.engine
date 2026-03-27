"""
Enumerations
============

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

Type stubs for :mod:`openswmm.engine._enums`.

Integer-backed enums mirroring the C API enum definitions in
``openswmm_engine.h``.
"""

from enum import IntEnum


class ErrorCode(IntEnum):
    """SWMM C API return codes.

    Attributes:
        OK: Success (0).
        NOMEM: Out of memory.
        INPFILE: Cannot open input file.
        RPTFILE: Cannot open report file.
        OUTFILE: Cannot open output file.
        PARSE: Input file parse error.
        LIFECYCLE: Function called in wrong lifecycle state.
        BADHANDLE: NULL or invalid engine handle.
        BADINDEX: Object index out of range.
        BADPARAM: Invalid parameter value.
        PLUGIN: Plugin error.
        IO: I/O error.
        HOTSTART: Hot start file error.
        CRS: Coordinate reference system error.
        NUMERICAL: Numerical error.
        INTERNAL: Internal error.
    """

    OK = 0
    NOMEM = 1
    INPFILE = 2
    RPTFILE = 3
    OUTFILE = 4
    PARSE = 5
    LIFECYCLE = 6
    BADHANDLE = 7
    BADINDEX = 8
    BADPARAM = 9
    PLUGIN = 10
    IO = 11
    HOTSTART = 12
    CRS = 13
    NUMERICAL = 14
    INTERNAL = 99


class EngineState(IntEnum):
    """Engine lifecycle states.

    Attributes:
        NONE: Not yet created.
        CREATED: Engine allocated but not opened.
        OPENED: Input file parsed.
        INITIALIZED: Arrays allocated, initial conditions set.
        STARTED: Simulation started.
        RUNNING: Simulation in progress (after first step).
        ENDED: Simulation ended.
        CLOSED: Files closed.
        BUILDING: Programmatic model construction in progress.
    """

    NONE = 0
    CREATED = 1
    OPENED = 2
    INITIALIZED = 3
    STARTED = 4
    RUNNING = 5
    ENDED = 6
    CLOSED = 7
    BUILDING = 8


class NodeType(IntEnum):
    """Node type codes for :meth:`ModelBuilder.add_node`.

    Attributes:
        JUNCTION: Junction node.
        OUTFALL: Outfall node.
        STORAGE: Storage unit.
        DIVIDER: Flow divider.
    """

    JUNCTION = 0
    OUTFALL = 1
    STORAGE = 2
    DIVIDER = 3


class LinkType(IntEnum):
    """Link type codes for :meth:`ModelBuilder.add_link`.

    Attributes:
        CONDUIT: Conduit (pipe/channel).
        PUMP: Pump.
        ORIFICE: Orifice.
        WEIR: Weir.
        OUTLET: Outlet.
    """

    CONDUIT = 0
    PUMP = 1
    ORIFICE = 2
    WEIR = 3
    OUTLET = 4


class XSectShape(IntEnum):
    """Cross-section shape codes for :meth:`ModelBuilder.set_link_xsect`.

    Attributes:
        CIRCULAR: Circular pipe.
        FILLED_CIRCULAR: Filled circular pipe.
        RECT_CLOSED: Closed rectangular.
        RECT_OPEN: Open rectangular.
        TRAPEZOIDAL: Trapezoidal channel.
        TRIANGULAR: Triangular channel.
        PARABOLIC: Parabolic channel.
        POWER: Power-law shaped channel.
        MODBASKETHANDLE: Modified baskethandle.
        EGGSHAPED: Egg-shaped pipe.
        HORSESHOE: Horseshoe-shaped pipe.
        GOTHIC: Gothic arch pipe.
        CATENARY: Catenary-shaped pipe.
        SEMIELLIPTICAL: Semi-elliptical pipe.
        BASKETHANDLE: Baskethandle-shaped pipe.
        SEMICIRCULAR: Semi-circular pipe.
        IRREGULAR: Irregular (from transect data).
        CUSTOM: Custom shape (from shape curve).
        FORCE_MAIN: Force main (pressurized).
    """

    CIRCULAR = 0
    FILLED_CIRCULAR = 1
    RECT_CLOSED = 2
    RECT_OPEN = 3
    TRAPEZOIDAL = 4
    TRIANGULAR = 5
    PARABOLIC = 6
    POWER = 7
    MODBASKETHANDLE = 8
    EGGSHAPED = 9
    HORSESHOE = 10
    GOTHIC = 11
    CATENARY = 12
    SEMIELLIPTICAL = 13
    BASKETHANDLE = 14
    SEMICIRCULAR = 15
    IRREGULAR = 16
    CUSTOM = 17
    FORCE_MAIN = 18


class FlowUnits(IntEnum):
    """Flow unit systems.

    Attributes:
        CFS: Cubic feet per second (US).
        GPM: Gallons per minute (US).
        MGD: Million gallons per day (US).
        CMS: Cubic meters per second (SI).
        LPS: Liters per second (SI).
        MLD: Million liters per day (SI).
    """

    CFS = 0
    GPM = 1
    MGD = 2
    CMS = 3
    LPS = 4
    MLD = 5


class RouteModel(IntEnum):
    """Hydraulic routing models.

    Attributes:
        STEADY: Steady-state routing.
        KINWAVE: Kinematic wave routing.
        DYNWAVE: Dynamic wave routing.
    """

    STEADY = 0
    KINWAVE = 1
    DYNWAVE = 2
