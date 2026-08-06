"""
Engine lifecycle (Pythonic v1 surface)
======================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

The :class:`Solver` class is the entry point for running, inspecting, and
editing a SWMM model from Python.

Compared to the pre-v1 surface:

* Lifecycle methods (:meth:`open`, :meth:`initialize`, :meth:`start`,
  :meth:`end`, :meth:`report`, :meth:`close`) **raise** on failure instead
  of returning an integer code. :meth:`step` / :meth:`stride` return a
  :class:`datetime.timedelta` (``timedelta(0)`` once the simulation ends).
* :attr:`Solver.state` returns an :class:`EngineState` enum value.
* :attr:`Solver.elapsed` and :attr:`Solver.routing_step` return
  :class:`datetime.timedelta`. :attr:`Solver.current_datetime`,
  :attr:`Solver.start_datetime`, :attr:`Solver.end_datetime`, and
  :attr:`Solver.report_start_datetime` return :class:`datetime.datetime`.
* :meth:`Solver.steps` and :meth:`Solver.until` provide explicit iteration
  helpers; bare ``iter(solver)`` is intentionally not provided.
* :attr:`Solver.options`, :attr:`Solver.userflags`, :attr:`Solver.events`
  are view objects (``MutableMapping`` / ``MutableSequence``) that replace
  the prior free-floating ``get_option`` / ``userflag_*`` / ``events_*``
  methods.
* :attr:`Solver.nodes`, :attr:`Solver.links`, ... are lazy collection
  accessors. In P1 they return the existing collection classes; full
  wrapper-object treatment lands in P2-P8.
* :attr:`Solver.surface2d` is the lazy :class:`Surface2D` view over the
  2D overland-flow mesh (check :attr:`Surface2D.is_active` before use).

.. code-block:: python

    from datetime import timedelta
    from pathlib import Path
    from openswmm.engine import Solver, EngineState

    with Solver(Path("model.inp"), Path("model.rpt"), Path("model.out")) as s:
        print(s.start_datetime, s.routing_step)
        for elapsed in s.steps():
            if elapsed >= timedelta(hours=24):
                break
"""

# cython: language_level=3

import os
from collections.abc import MutableMapping, MutableSequence
from datetime import datetime, timedelta
from typing import Iterator, Optional, Union

from ._common cimport *
from ._dates import datetime_to_oadate, oadate_to_datetime
from ._enums import EngineState, FlowUnits

# Canonical EngineError hierarchy lives in :mod:`_exceptions`. Re-exported for
# both intra-module raises and backward-compatible imports.
from ._exceptions import (
    EngineError,
    BadHandleError,
    BadIndexError,
    BadParamError,
    LifecycleError,
    HotStartError,
    PluginError,
    FileError,
    ParseError,
    NumericalError,
    CRSError,
    DependencyError,
    StaleObjectError,
)


# =============================================================================
# Helpers
# =============================================================================

_SECONDS_PER_DAY = 86400.0

#: Registered id of the built-in GeoPackage model writer/reader plugin. Pass to
#: :meth:`Solver.write_with_plugin` (or use :meth:`Solver.write_geopackage`).
GEOPACKAGE_PLUGIN_ID = "org.hydrocouple.openswmm.plugins.geopackage"


def _path_to_str(p) -> str:
    """Accept ``str``, ``os.PathLike``, or ``None``; return a ``str`` (empty
    when input is None). Used to support :class:`pathlib.Path` arguments."""
    if p is None:
        return ""
    return os.fspath(p)


cdef inline double _td_to_days(object td):
    """Convert a :class:`timedelta` (or numeric) to decimal days."""
    if isinstance(td, timedelta):
        return td.total_seconds() / _SECONDS_PER_DAY
    return float(td) / _SECONDS_PER_DAY  # accept raw seconds as a courtesy


cdef inline object _days_to_td(double days):
    """Convert decimal days to a :class:`timedelta`."""
    return timedelta(seconds=days * _SECONDS_PER_DAY)


# =============================================================================
# C trampolines for step callbacks
# =============================================================================

cdef void _step_begin_trampoline(SWMM_Engine engine, double sim_time,
                                  double dt, void* user_data) noexcept with gil:
    (<object>user_data)(sim_time, dt)


cdef void _step_end_trampoline(SWMM_Engine engine, double sim_time,
                                double dt, void* user_data) noexcept with gil:
    (<object>user_data)(sim_time, dt)


cdef void _progress_trampoline(void* engine, double elapsed_frac,
                               double sim_time, void* user_data) noexcept with gil:
    (<object>user_data)(elapsed_frac)


cdef void _warning_trampoline(SWMM_Engine engine, int code, const char* msg,
                               void* user_data) noexcept with gil:
    cdef bytes b = msg if msg is not NULL else b""
    (<object>user_data)(code, b.decode("utf-8", "replace"))


# =============================================================================
# Solver
# =============================================================================

cdef class Solver:
    """SWMM engine lifecycle and entry point to every domain accessor.

    :param inp: Path to the SWMM input file (``.inp``). Accepts ``str``,
        :class:`pathlib.Path`, or any :class:`os.PathLike`.
    :param rpt: Path for the report file (``.rpt``). ``None`` skips
        reporting.
    :param out: Path for the binary output file (``.out``). ``None`` skips.
    :param plugin_lib: Optional path to a plugin shared library, loaded
        before parsing.

    The Solver supports the context manager protocol; in the typical case
    the only code you write is the simulation loop:

    .. code-block:: python

        from datetime import timedelta
        from openswmm.engine import Solver, EngineState

        with Solver("model.inp", "model.rpt", "model.out") as s:
            for elapsed in s.steps():
                if elapsed >= timedelta(hours=24):
                    break

    On entry the solver runs ``open → initialize → start``; on exit it runs
    ``end → report → close → destroy``. Any non-zero return from the C API
    raises an :class:`EngineError` subclass (see :doc:`error_handling`).
    """

    def __init__(self,
                 inp="",
                 rpt=None,
                 out=None,
                 *,
                 plugin_lib: Optional[Union[str, "os.PathLike"]] = None):
        self._inp = _path_to_str(inp)
        self._rpt = _path_to_str(rpt)
        self._out = _path_to_str(out)
        self._elapsed = 0.0
        self._handle = NULL
        self._step_begin_cb = None
        self._step_end_cb = None
        self._warning_cb = None
        self._progress_cb = None
        self._options = None
        self._userflags = None
        self._events_view = None
        self._nodes = None
        self._links = None
        self._subcatchments = None
        self._aquifers = None
        self._snowpacks = None
        self._gages = None
        self._pollutants = None
        self._tables = None
        self._patterns = None
        self._inflows = None
        self._controls = None
        self._forcing = None
        self._climate = None
        self._infrastructure = None
        self._spatial = None
        self._quality = None
        self._statistics = None
        self._mass_balance = None
        self._editor = None
        self._hotstart = None
        self._surface2d = None
        self._generation = 0
        # Stash the plugin_lib for ``open()`` to consume. We don't pass it to
        # __init__ purely for symmetry with the v0 surface (which took it via
        # ``open(plugin_lib=...)``) — both call shapes are now supported.
        self._plugin_lib = _path_to_str(plugin_lib) if plugin_lib else None

    # Non-cdef attribute — declared here, not in the pxd, so Python attribute
    # assignment works inside __init__. Cython lets us mix cdef and pure-Python
    # attributes on a cdef class as long as the class declares __dict__.
    # (We rely on the default behaviour — cdef classes without `cdef dict` get
    #  __slots__-like semantics, so this lives as a Python attribute via
    #  __init__-side __dict__ activation if we needed it; in practice we use
    #  the explicit cdef slot _plugin_lib instead. Declared inline below.)

    # ------------------------------------------------------------------
    # Lifecycle — raise on failure, no integer return codes
    # ------------------------------------------------------------------

    def create(self) -> None:
        """Allocate the engine handle. Usually called implicitly by
        :meth:`open` or the context manager."""
        if self._handle != NULL:
            return
        self._handle = swmm_engine_create()
        if self._handle == NULL:
            raise MemoryError("Failed to allocate SWMM engine handle")

    def set_lenient_open(self, bint on=True) -> None:
        """Enable or disable permissive ("lenient") open on this engine.

        When enabled, a subsequent :meth:`open` records post-parse validation
        errors (undefined objects, missing curves, bad references) but still
        leaves the engine ``OPENED`` with all parsed objects intact and
        editable, instead of failing. This is intended for editor/GUI loads
        that must show as much of a broken model as possible; the accumulated
        errors are then readable via :attr:`open_errors`. Hard reader failures
        still fail the open. The default is strict. Running a model still needs
        a fresh, strict open.

        :param on: ``True`` to enable lenient open, ``False`` for strict.
        :raises EngineError: If the engine handle cannot be allocated.
        """
        if self._handle == NULL:
            self.create()
        swmm_engine_set_lenient_open(self._handle, 1 if on else 0)

    def open(self,
             plugin_lib: Optional[Union[str, "os.PathLike"]] = None) -> None:
        """Open the input file; transition to ``OPENED``.

        :param plugin_lib: Override the ``plugin_lib`` passed to
            :meth:`__init__`. Pass ``None`` to use the constructor value.
        :raises EngineError: On C API failure (specific subclass per code).
        """
        if self._handle == NULL:
            self.create()
        cdef bytes b_inp = self._inp.encode('utf-8')
        cdef bytes b_rpt = self._rpt.encode('utf-8')
        cdef bytes b_out = self._out.encode('utf-8')
        cdef bytes b_plugin
        cdef const char* c_plugin = NULL
        resolved_plugin = (
            _path_to_str(plugin_lib) if plugin_lib else self._plugin_lib
        )
        if resolved_plugin:
            b_plugin = resolved_plugin.encode('utf-8')
            c_plugin = b_plugin
        _check(swmm_engine_open(self._handle, b_inp, b_rpt, b_out, c_plugin))

    def initialize(self) -> None:
        """Initialize the simulation; transition to ``INITIALIZED``."""
        _check(swmm_engine_initialize(self._handle))

    def start(self, bint save_results=True) -> None:
        """Start the simulation; transition to ``STARTED``.

        :param save_results: If ``True``, write binary output to the
            ``.out`` file. When ``False`` the file is not produced.
        """
        _check(swmm_engine_start(self._handle, 1 if save_results else 0))

    def step(self) -> timedelta:
        """Advance one routing step.

        :returns: The elapsed simulation time after the step as a
            :class:`timedelta`. ``timedelta(0)`` indicates the simulation
            has ended — callers should break out of their loop.
        :raises EngineError: On any non-zero C API return.
        """
        cdef double elapsed = 0.0
        cdef SWMM_Engine h = self._handle
        cdef int rc
        with nogil:
            rc = swmm_engine_step(h, &elapsed)
        _check(rc)
        self._elapsed = elapsed
        return _days_to_td(elapsed)

    def stride(self, int n_steps) -> timedelta:
        """Advance ``n_steps`` routing steps in one call.

        :returns: Elapsed simulation time after the last step taken, as a
            :class:`timedelta`. ``timedelta(0)`` once the simulation has
            ended.
        """
        cdef double elapsed = 0.0
        cdef SWMM_Engine h = self._handle
        cdef int rc
        with nogil:
            rc = swmm_engine_stride(h, n_steps, &elapsed)
        _check(rc)
        self._elapsed = elapsed
        return _days_to_td(elapsed)

    def end(self) -> None:
        """End the simulation; transition to ``ENDED``."""
        _check(swmm_engine_end(self._handle))

    def report(self) -> None:
        """Write the summary report to the ``.rpt`` file."""
        _check(swmm_engine_report(self._handle))

    def close(self) -> None:
        """Close all files; transition to ``CLOSED``. Idempotent."""
        if self._handle == NULL:
            return
        _check(swmm_engine_close(self._handle))

    def destroy(self) -> None:
        """Destroy the engine handle. Idempotent."""
        if self._handle != NULL:
            swmm_engine_destroy(self._handle)
            self._handle = NULL

    # ------------------------------------------------------------------
    # Iteration helpers
    # ------------------------------------------------------------------

    def steps(self) -> Iterator[timedelta]:
        """Iterate routing steps until the simulation ends.

        Each iteration yields the elapsed :class:`timedelta` after the
        most recent step. The loop terminates automatically when the
        engine reports zero elapsed time:

        .. code-block:: python

            from datetime import timedelta
            with Solver("model.inp") as s:
                for elapsed in s.steps():
                    if elapsed >= timedelta(hours=24):
                        break

        :raises EngineError: Propagated from :meth:`step`.
        """
        cdef double elapsed = 0.0
        cdef SWMM_Engine h = self._handle
        cdef int rc
        while True:
            with nogil:
                rc = swmm_engine_step(h, &elapsed)
            _check(rc)
            self._elapsed = elapsed
            if elapsed <= 0.0:
                return
            yield _days_to_td(elapsed)

    def until(self, target) -> timedelta:
        """Stride forward until the engine reaches ``target``.

        :param target: Either a :class:`datetime.datetime` (an absolute
            target moment) or a :class:`datetime.timedelta` (an elapsed
            duration measured from the simulation start). Naive datetimes
            are interpreted in the simulation's native frame.
        :returns: The actual elapsed :class:`timedelta` reached. May be
            **less than** the requested target when the simulation ends
            first; callers should check the return value (or
            :attr:`state`) to detect end-of-run.

        Internally this calls :meth:`step` in a tight loop. Use it when
        you want "advance to noon on day 3" semantics:

        .. code-block:: python

            from datetime import timedelta
            with Solver("model.inp") as s:
                s.until(timedelta(hours=12))
                snapshot = s.nodes.depths.copy()    # state at t=12h
        """
        # Resolve target -> required elapsed days from the simulation start.
        cdef double target_days
        if isinstance(target, timedelta):
            target_days = target.total_seconds() / _SECONDS_PER_DAY
        elif isinstance(target, datetime):
            start_dt = self.start_datetime
            delta = target - start_dt
            target_days = delta.total_seconds() / _SECONDS_PER_DAY
        else:
            raise TypeError(
                "until(target) expects datetime or timedelta, got "
                + type(target).__name__
            )
        if target_days <= self._elapsed:
            return _days_to_td(self._elapsed)

        cdef double elapsed = self._elapsed
        cdef SWMM_Engine h = self._handle
        cdef int rc
        while elapsed > 0.0 or self._elapsed == 0.0:
            with nogil:
                rc = swmm_engine_step(h, &elapsed)
            _check(rc)
            self._elapsed = elapsed
            if elapsed <= 0.0:                        # simulation ended
                break
            if elapsed >= target_days:                # reached the target
                break
        return _days_to_td(self._elapsed)

    # ------------------------------------------------------------------
    # Convenience: run-to-completion
    # ------------------------------------------------------------------

    def run(self) -> None:
        """Run the full simulation lifecycle in one call.

        Equivalent to::

            self.open()
            self.initialize()
            self.start()
            for _ in self.steps():
                pass
            self.end()
            self.report()
            self.close()

        Useful as a non-context-manager convenience when you have nothing
        to do between steps. The free function :func:`run` accepts the
        same arguments via the constructor; this method is a method-form
        equivalent for clients that already hold a :class:`Solver`.
        """
        self.open()
        self.initialize()
        self.start()
        for _ in self.steps():
            pass
        self.end()
        try:
            self.report()
        finally:
            self.close()

    # ------------------------------------------------------------------
    # State, timing properties (typed)
    # ------------------------------------------------------------------

    @property
    def elapsed(self) -> timedelta:
        """Elapsed simulation time after the last :meth:`step` /
        :meth:`stride`, as a :class:`timedelta`."""
        return _days_to_td(self._elapsed)

    @property
    def state(self) -> EngineState:
        """Current lifecycle state as an :class:`EngineState` enum."""
        cdef int s = 0
        _check(swmm_engine_get_state(self._handle, &s))
        return EngineState(s)

    @property
    def handle(self) -> int:
        """Raw C engine handle as an integer pointer value.

        Provided for advanced interop (e.g. passing the handle to other
        Cython modules). The Python-level surface should never need it.
        """
        return <size_t>self._handle

    @property
    def open_errors(self) -> list:
        """Validation errors accumulated on the engine during :meth:`open`.

        Populated primarily after a lenient open (see
        :meth:`set_lenient_open`); a strict open that succeeds leaves this
        empty. Each entry is a human-readable message string.

        :rtype: list[str]
        """
        cdef int n = swmm_get_error_count(self._handle)
        cdef int i
        cdef const char* msg
        cdef list out = []
        for i in range(n):
            msg = swmm_get_error_at(self._handle, i)
            out.append(msg.decode('utf-8') if msg != NULL else "")
        return out

    @property
    def open_warnings(self) -> list:
        """Warnings accumulated on the engine during :meth:`open`.

        Each entry is a human-readable message string.

        :rtype: list[str]
        """
        cdef int n = swmm_get_warning_count(self._handle)
        cdef int i
        cdef const char* msg
        cdef list out = []
        for i in range(n):
            msg = swmm_get_warning_at(self._handle, i)
            out.append(msg.decode('utf-8') if msg != NULL else "")
        return out

    @property
    def generation(self) -> int:
        """Monotonic counter bumped on every structural mutation (add /
        delete / rename). Wrapper objects in P2+ use this to detect
        staleness."""
        return int(self._generation)

    def _bump_generation(self) -> None:
        """Increment the staleness counter. Called by collection-level
        editors (``solver.nodes.add``, ``rename``, ``delete``, …) so
        that wrappers minted before the mutation can detect they are
        out of date. Internal: do not call directly."""
        self._generation += 1

    @property
    def routing_step(self) -> timedelta:
        """Routing timestep as a :class:`timedelta`."""
        cdef double v = 0.0
        _check(swmm_get_routing_step(self._handle, &v))
        return timedelta(seconds=v)

    # ------------------------------------------------------------------
    # Datetime properties
    # ------------------------------------------------------------------

    @property
    def start_datetime(self) -> datetime:
        """Simulation start :class:`datetime.datetime`."""
        cdef double v = 0.0
        _check(swmm_options_get_start_date(self._handle, &v))
        return oadate_to_datetime(v)

    @start_datetime.setter
    def start_datetime(self, value: datetime) -> None:
        _check(swmm_options_set_start_date(self._handle, datetime_to_oadate(value)))

    @property
    def end_datetime(self) -> datetime:
        """Simulation end :class:`datetime.datetime`."""
        cdef double v = 0.0
        _check(swmm_options_get_end_date(self._handle, &v))
        return oadate_to_datetime(v)

    @end_datetime.setter
    def end_datetime(self, value: datetime) -> None:
        _check(swmm_options_set_end_date(self._handle, datetime_to_oadate(value)))

    @property
    def report_start_datetime(self) -> datetime:
        """Report start :class:`datetime.datetime`."""
        cdef double v = 0.0
        _check(swmm_options_get_report_start(self._handle, &v))
        return oadate_to_datetime(v)

    @report_start_datetime.setter
    def report_start_datetime(self, value: datetime) -> None:
        _check(swmm_options_set_report_start(self._handle, datetime_to_oadate(value)))

    @property
    def current_datetime(self) -> datetime:
        """Current simulation :class:`datetime.datetime`.

        Equivalent to ``start_datetime + elapsed``. After the simulation
        ends this returns the end-of-simulation moment.
        """
        # swmm_get_current_time returns elapsed *seconds* from the start
        # (context().current_time), not an absolute OADate, so feeding it to
        # oadate_to_datetime yields the 1899 epoch. Build the absolute moment
        # from the (already-correct) start_datetime + elapsed timedelta, which
        # is exactly the documented contract.
        return self.start_datetime + self.elapsed

    @property
    def sim_start_time(self) -> datetime:
        """Resolved simulation start as a :class:`datetime.datetime`.

        The engine's resolved start of the routing window (from
        ``swmm_get_start_time``), as distinct from the configured
        :attr:`start_datetime` option.
        """
        cdef double v = 0.0
        _check(swmm_get_start_time(self._handle, &v))
        return oadate_to_datetime(v)

    @property
    def sim_end_time(self) -> datetime:
        """Resolved simulation end as a :class:`datetime.datetime`.

        The engine's resolved end of the routing window (from
        ``swmm_get_end_time``).
        """
        cdef double v = 0.0
        _check(swmm_get_end_time(self._handle, &v))
        return oadate_to_datetime(v)

    @property
    def event_count(self) -> int:
        """Number of ``[EVENTS]`` entries defined on the model.

        @rtype: int
        """
        cdef int v = 0
        _check(swmm_get_event_count(self._handle, &v))
        return v

    # ------------------------------------------------------------------
    # CRS
    # ------------------------------------------------------------------

    @property
    def crs(self) -> str:
        """Coordinate reference system string from ``[OPTIONS]``."""
        cdef char buf[256]
        _check(swmm_get_crs(self._handle, buf, 256))
        return buf.decode('utf-8')

    # ------------------------------------------------------------------
    # Steady-state skip
    # ------------------------------------------------------------------

    @property
    def steady_state_skip(self) -> bool:
        cdef int v = 0
        _check(swmm_get_steady_state_skip(self._handle, &v))
        return v != 0

    @steady_state_skip.setter
    def steady_state_skip(self, value: bool) -> None:
        _check(swmm_set_steady_state_skip(self._handle, 1 if value else 0))

    @property
    def is_between_events(self) -> bool:
        """``True`` when the current routing time falls outside any
        ``[EVENTS]`` window (i.e. routing is being skipped)."""
        cdef int v = 0
        _check(swmm_is_between_events(self._handle, &v))
        return v != 0

    # ------------------------------------------------------------------
    # Model write
    # ------------------------------------------------------------------

    def write(self, path) -> None:
        """Write the current model to a SWMM ``.inp`` file."""
        cdef bytes b = _path_to_str(path).encode('utf-8')
        _check(swmm_model_write(self._handle, b))

    def write_with_plugin(self, path, str output_plugin_id="") -> None:
        """Write the current model using an output plugin.

        Pass an empty ``output_plugin_id`` (the default) for the built-in
        ``.inp`` writer; pass a registered plugin id (e.g.
        :data:`GEOPACKAGE_PLUGIN_ID`) to serialise the model with that plugin.

        @param path: Destination file path.
        @param output_plugin_id: Plugin id, or ``""`` for the built-in writer.
        @raise EngineError: On C API failure (e.g. unknown plugin id).
        """
        cdef bytes bp = _path_to_str(path).encode('utf-8')
        cdef bytes bid = output_plugin_id.encode('utf-8')
        _check(swmm_model_write_with_plugin(self._handle, bp, bid))

    def write_geopackage(self, path, crs=None) -> None:
        """Write the model to an OGC GeoPackage (``.gpkg``).

        Convenience wrapper over :meth:`write_with_plugin` using the built-in
        GeoPackage writer. Network nodes, links, subcatchments and rain gages
        are written as feature layers.

        @param path: Destination ``.gpkg`` path.
        @param crs: Optional coordinate reference system string (e.g.
            ``"EPSG:2284"``). When given it is applied via
            ``solver.spatial.crs`` first, so every feature is tagged with that
            SRS — without it the geometries are written with an undefined SRS
            and GIS tools cannot place them. Pass ``None`` to keep the model's
            existing CRS.
        @raise EngineError: On C API failure.
        """
        if crs is not None:
            self.spatial.crs = crs
        self.write_with_plugin(path, GEOPACKAGE_PLUGIN_ID)

    # ------------------------------------------------------------------
    # Views: options / userflags / events
    # ------------------------------------------------------------------

    @property
    def options(self):
        """``solver.options`` — a :class:`SimulationOptions` view.

        Exposes string-keyed ``[OPTIONS]`` access (``solver.options[key]``)
        plus typed properties for the dates / routing step / CRS. Cached
        on first access.
        """
        if self._options is None:
            self._options = SimulationOptions(self)
        return self._options

    @property
    def flow_units(self):
        """``solver.flow_units`` — the model's flow-unit system.

        Returns a :class:`~openswmm.engine.FlowUnits` enum member via the
        engine's typed ``swmm_get_flow_units`` accessor. This is the
        first-class way to discover the units a running model exchanges:
        because the C API returns every quantity in the units declared in
        the ``.inp`` file (project units), consumers should read this to
        interpret returned magnitudes correctly.

        @return: The active flow-unit system.
        @rtype: L{openswmm.engine.FlowUnits}
        @raise EngineError: On C API failure.
        """
        cdef int v = 0
        _check(swmm_get_flow_units(self._handle, &v))
        return FlowUnits(v)

    @property
    def unit_system(self):
        """``solver.unit_system`` — ``'US'`` or ``'SI'``.

        Returns the engine's typed ``swmm_get_unit_system`` result
        (``0`` = US/imperial, ``1`` = SI/metric). ``CFS``/``GPM``/``MGD``
        are US customary; ``CMS``/``LPS``/``MLD`` are SI. Length, area, and
        volume getters return values in the corresponding project units.

        @return: ``'US'`` for US-customary models, ``'SI'`` for metric.
        @rtype: str
        @raise EngineError: On C API failure.
        """
        cdef int v = 0
        _check(swmm_get_unit_system(self._handle, &v))
        return "SI" if v == 1 else "US"

    @property
    def userflags(self):
        """``solver.userflags`` — a :class:`UserFlags` mapping.

        Reads / writes user-defined flags by name. Assigning a Python
        ``bool``, ``int``, or ``float`` chooses the matching C setter.
        """
        if self._userflags is None:
            self._userflags = UserFlags(self)
        return self._userflags

    @property
    def events(self):
        """``solver.events`` — a :class:`EventsView` ``MutableSequence``.

        ``[EVENTS]`` rows as :class:`Event` records; supports indexing,
        slicing (read-only slices), ``append``, ``insert``, ``__delitem__``,
        and ``clear``.
        """
        if self._events_view is None:
            self._events_view = EventsView(self)
        return self._events_view

    # ------------------------------------------------------------------
    # Collection accessors — stubbed in P1, full wrappers in P2-P8
    # ------------------------------------------------------------------

    @property
    def nodes(self):
        """``solver.nodes`` — node collection. In P1 returns the legacy
        :class:`Nodes` helper; the wrapper-object collection lands in P2."""
        if self._nodes is None:
            from ._nodes import Nodes
            self._nodes = Nodes(self)
        return self._nodes

    @property
    def links(self):
        if self._links is None:
            from ._links import Links
            self._links = Links(self)
        return self._links

    @property
    def subcatchments(self):
        if self._subcatchments is None:
            from ._subcatchments import Subcatchments
            self._subcatchments = Subcatchments(self)
        return self._subcatchments

    @property
    def aquifers(self):
        """``solver.aquifers`` — :class:`Aquifers` collection of ``[AQUIFERS]``."""
        if self._aquifers is None:
            from ._subcatchments import Aquifers
            self._aquifers = Aquifers(self)
        return self._aquifers

    @property
    def snowpacks(self):
        """``solver.snowpacks`` — :class:`Snowpacks` collection of ``[SNOWPACKS]``."""
        if self._snowpacks is None:
            from ._subcatchments import Snowpacks
            self._snowpacks = Snowpacks(self)
        return self._snowpacks

    @property
    def gages(self):
        if self._gages is None:
            from ._gages import Gages
            self._gages = Gages(self)
        return self._gages

    @property
    def pollutants(self):
        if self._pollutants is None:
            from ._pollutants import Pollutants
            self._pollutants = Pollutants(self)
        return self._pollutants

    @property
    def tables(self):
        if self._tables is None:
            from ._tables import Tables
            self._tables = Tables(self)
        return self._tables

    @property
    def patterns(self):
        """``solver.patterns`` — :class:`Patterns` collection."""
        if self._patterns is None:
            from ._tables import Patterns
            self._patterns = Patterns(self)
        return self._patterns

    @property
    def inflows(self):
        if self._inflows is None:
            from ._inflows import Inflows
            self._inflows = Inflows(self)
        return self._inflows

    @property
    def controls(self):
        if self._controls is None:
            from ._controls import Controls
            self._controls = Controls(self)
        return self._controls

    @property
    def forcing(self):
        if self._forcing is None:
            from ._forcing import Forcing
            self._forcing = Forcing(self)
        return self._forcing

    @property
    def climate(self):
        if self._climate is None:
            from ._climate import Climate
            self._climate = Climate(self)
        return self._climate

    @property
    def infrastructure(self):
        if self._infrastructure is None:
            from ._infrastructure import Infrastructure
            self._infrastructure = Infrastructure(self)
        return self._infrastructure

    @property
    def spatial(self):
        if self._spatial is None:
            from ._spatial import Spatial
            self._spatial = Spatial(self)
        return self._spatial

    @property
    def quality(self):
        if self._quality is None:
            from ._quality import Quality
            self._quality = Quality(self)
        return self._quality

    @property
    def statistics(self):
        if self._statistics is None:
            from ._statistics import Statistics
            self._statistics = Statistics(self)
        return self._statistics

    @property
    def mass_balance(self):
        if self._mass_balance is None:
            from ._massbalance import MassBalance
            self._mass_balance = MassBalance(self)
        return self._mass_balance

    @property
    def editor(self):
        if self._editor is None:
            from ._edit import ModelEditor
            self._editor = ModelEditor(self)
        return self._editor

    @property
    def save_schedule(self):
        """``solver.save_schedule`` — :class:`MutableSequence` over the
        ``[SAVE HOTSTART]`` block. Entries are
        :class:`SaveScheduleEntry` records carrying ``when: datetime``
        and ``path: str``."""
        if self._hotstart is None:
            from ._hotstart import SaveSchedule
            self._hotstart = SaveSchedule(self)
        return self._hotstart

    @property
    def surface2d(self):
        """``solver.surface2d`` — the :class:`Surface2D` overland-flow view.

        Mesh queries, per-triangle state, statistics, forcing, edge
        boundary conditions, and edge conveyance for the 2D diffusion-wave
        surface. Check :meth:`Surface2D.is_active` before use — a model
        with no ``[2D_*]`` sections has an inactive surface.

        @return: The cached L{Surface2D} view for this solver's engine.
        @rtype: Surface2D
        @raise ImportError: When the extension was built without 2D
            support.
        """
        if self._surface2d is None:
            from ._2d import Surface2D
            self._surface2d = Surface2D(self.handle)
        return self._surface2d

    # ------------------------------------------------------------------
    # Runoff interface file
    # ------------------------------------------------------------------

    def open_runoff_interface_write(self, path) -> None:
        """Open the runoff interface file in SAVE mode."""
        cdef bytes b = _path_to_str(path).encode('utf-8')
        cdef SWMM_Engine h = self._handle
        cdef const char* p = b
        cdef int err
        with nogil:
            err = swmm_runoff_iface_open_write(h, p)
        _check(err)

    def open_runoff_interface_read(self, path) -> None:
        """Open the runoff interface file in USE mode."""
        cdef bytes b = _path_to_str(path).encode('utf-8')
        cdef SWMM_Engine h = self._handle
        cdef const char* p = b
        cdef int err
        with nogil:
            err = swmm_runoff_iface_open_read(h, p)
        _check(err)

    def save_runoff_step(self, dt) -> None:
        """Force one runoff substep snapshot to the open SAVE file.

        :param dt: Substep duration. Accepts :class:`timedelta` or a
            number of seconds.
        """
        cdef double dt_s
        if isinstance(dt, timedelta):
            dt_s = dt.total_seconds()
        else:
            dt_s = float(dt)
        cdef SWMM_Engine h = self._handle
        cdef int err
        with nogil:
            err = swmm_runoff_iface_save_step(h, dt_s)
        _check(err)

    def read_runoff_step(self) -> bool:
        """Read one substep from the USE-mode runoff interface file.

        :returns: ``True`` when a record was read, ``False`` on EOF.
        """
        cdef SWMM_Engine h = self._handle
        cdef int has = 0
        cdef int err
        with nogil:
            err = swmm_runoff_iface_read_step(h, &has)
        _check(err)
        return bool(has)

    def close_runoff_interface(self) -> None:
        """Close the runoff interface file. Idempotent."""
        cdef SWMM_Engine h = self._handle
        cdef int err
        with nogil:
            err = swmm_runoff_iface_close(h)
        _check(err)

    # ------------------------------------------------------------------
    # Callbacks
    # ------------------------------------------------------------------

    def set_step_begin_callback(self, callback) -> None:
        """Register a callback invoked at the start of each timestep.

        The callable receives ``(sim_time: float, dt: float)`` where
        ``sim_time`` is the current SWMM DateTime double and ``dt`` is
        seconds. Pass ``None`` to unregister.
        """
        if callback is None:
            self._step_begin_cb = None
            _check(swmm_set_step_begin_callback(self._handle, NULL, NULL))
        else:
            self._step_begin_cb = callback
            _check(swmm_set_step_begin_callback(
                self._handle, _step_begin_trampoline,
                <void*>self._step_begin_cb))

    def set_step_end_callback(self, callback) -> None:
        """Register a callback invoked at the end of each timestep."""
        if callback is None:
            self._step_end_cb = None
            _check(swmm_set_step_end_callback(self._handle, NULL, NULL))
        else:
            self._step_end_cb = callback
            _check(swmm_set_step_end_callback(
                self._handle, _step_end_trampoline,
                <void*>self._step_end_cb))

    def set_warning_callback(self, callback) -> None:
        """Register a callback invoked on each non-fatal engine warning.

        The callable receives ``(code: int, message: str)``. Pass ``None``
        to unregister.
        """
        if callback is None:
            self._warning_cb = None
            _check(swmm_set_warning_callback(self._handle, NULL, NULL))
        else:
            self._warning_cb = callback
            _check(swmm_set_warning_callback(
                self._handle, _warning_trampoline,
                <void*>self._warning_cb))

    def set_progress_callback(self, callback) -> None:
        """Register a callback invoked as the simulation advances.

        The callable receives a single ``float`` — the elapsed fraction of
        the simulation in ``[0.0, 1.0]`` — suitable for driving a progress
        bar. Pass ``None`` to unregister.

        @param callback: A callable ``(elapsed_frac: float) -> None``, or
            ``None`` to clear.
        """
        if callback is None:
            self._progress_cb = None
            _check(swmm_set_progress_callback(self._handle, NULL, NULL))
        else:
            self._progress_cb = callback
            _check(swmm_set_progress_callback(
                self._handle, _progress_trampoline,
                <void*>self._progress_cb))

    # ------------------------------------------------------------------
    # Context manager
    # ------------------------------------------------------------------

    def __enter__(self):
        self.open()
        self.initialize()
        self.start()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        # Lifecycle teardown swallows per-step errors so destroy always runs.
        try:
            self.end()
        except EngineError:
            pass
        try:
            self.report()
        except EngineError:
            pass
        try:
            self.close()
        except EngineError:
            pass
        self.destroy()
        return False

    def __repr__(self) -> str:
        try:
            st = self.state.name
        except Exception:
            st = "?"
        return f"<Solver state={st} inp={self._inp!r}>"


# =============================================================================
# View classes
# =============================================================================

# A small named-tuple-like record so events surface as ``(start, end)``
# accessible by attribute or index.
from collections import namedtuple
Event = namedtuple("Event", ["start", "end"])

# A user-flag schema definition record: ``(name, type, description)``
# where ``type`` is 0=BOOLEAN, 1=INTEGER, 2=REAL, 3=STRING.
UserFlagDef = namedtuple("UserFlagDef", ["name", "type", "description"])


class SimulationOptions(MutableMapping):
    """``solver.options`` view.

    Acts as a :class:`MutableMapping` over the string-keyed ``[OPTIONS]``
    block, *and* exposes the typed properties below for the dates and
    routing parameters that are awkward as raw strings.

    String-keyed access:

    .. code-block:: python

        solver.options["FLOW_UNITS"]            # 'CFS'
        solver.options["FLOW_UNITS"] = "CMS"

    Typed shortcuts (for keys whose natural type is not ``str``):

    .. code-block:: python

        solver.options.start_datetime           # datetime
        solver.options.routing_step             # timedelta
    """

    def __init__(self, solver):
        self._solver = solver
        # Sub-namespace for the [OPTIONS_EXT] block.
        self.ext = _OptionsExtView(solver)

    # MutableMapping requires __getitem__, __setitem__, __delitem__, __iter__,
    # __len__. The underlying C API doesn't expose an iteration helper, so we
    # implement the mapping protocol minimally: indexing and length, plus an
    # ``iter`` over a small known-key set the schema documents. Iteration over
    # *all* keys requires a future C API addition.
    _KNOWN_KEYS = (
        "FLOW_UNITS", "INFILTRATION", "FLOW_ROUTING", "LINK_OFFSETS",
        "FORCE_MAIN_EQUATION", "IGNORE_RAINFALL", "IGNORE_SNOWMELT",
        "IGNORE_GROUNDWATER", "IGNORE_RDII", "IGNORE_ROUTING",
        "IGNORE_QUALITY", "ALLOW_PONDING", "SKIP_STEADY_STATE",
        "SYS_FLOW_TOL", "LAT_FLOW_TOL", "START_DATE", "START_TIME",
        "END_DATE", "END_TIME", "REPORT_START_DATE", "REPORT_START_TIME",
        "SWEEP_START", "SWEEP_END", "DRY_DAYS", "REPORT_STEP",
        "WET_STEP", "DRY_STEP", "ROUTING_STEP", "RULE_STEP",
        "INERTIAL_DAMPING", "NORMAL_FLOW_LIMITED", "MIN_SURFAREA",
        "MIN_SLOPE", "MAX_TRIALS", "HEAD_TOLERANCE", "THREADS",
        "TEMPDIR",
        # Explicit finite-volume solver (FLOW_ROUTING FV). Readable and
        # writable under any routing model, so a script can configure the
        # solver before selecting it.
        "FV_CELL_LENGTH", "FV_MIN_CELLS", "FV_CFL", "FV_RIEMANN", "FV_ORDER",
        "FV_LIMITER", "FV_SCALAR_SCHEME", "FV_TIME_INTEGRATION",
        "FV_SLOT_CELERITY", "FV_DISPERSION", "FV_STRUCTURE_COUPLING",
        "FV_COMPACTION", "FV_BACKEND", "FV_MIN_PARALLEL_CELLS",
    )

    def __getitem__(self, key: str) -> str:
        try:
            return _options_get(self._solver, key)
        except BadParamError as e:
            raise KeyError(key) from e

    def __setitem__(self, key: str, value) -> None:
        _options_set(self._solver, key, value if isinstance(value, str) else str(value))

    def __delitem__(self, key: str) -> None:
        raise TypeError("SWMM options cannot be deleted; assign an empty string instead")

    def __iter__(self):
        for k in self._KNOWN_KEYS:
            try:
                self[k]
            except KeyError:
                continue
            yield k

    def __len__(self) -> int:
        return sum(1 for _ in self)

    def __contains__(self, key) -> bool:
        if not isinstance(key, str):
            return False
        try:
            self[key]
            return True
        except KeyError:
            return False

    # Typed shortcuts mirror the Solver-level properties so users can write
    # ``solver.options.start_datetime`` as well as ``solver.start_datetime``.
    @property
    def start_datetime(self) -> datetime:
        return self._solver.start_datetime

    @start_datetime.setter
    def start_datetime(self, value: datetime) -> None:
        self._solver.start_datetime = value

    @property
    def end_datetime(self) -> datetime:
        return self._solver.end_datetime

    @end_datetime.setter
    def end_datetime(self, value: datetime) -> None:
        self._solver.end_datetime = value

    @property
    def report_start_datetime(self) -> datetime:
        return self._solver.report_start_datetime

    @report_start_datetime.setter
    def report_start_datetime(self, value: datetime) -> None:
        self._solver.report_start_datetime = value

    @property
    def routing_step(self) -> timedelta:
        return self._solver.routing_step

    @property
    def crs(self) -> str:
        return self._solver.crs

    def __repr__(self) -> str:
        return f"<SimulationOptions of {self._solver!r}>"


class _OptionsExtView(MutableMapping):
    """Sub-namespace for the ``[OPTIONS_EXT]`` block. Same protocol as
    :class:`SimulationOptions` but routes to the ``*_ext`` C entry points."""

    def __init__(self, solver):
        self._solver = solver

    def __getitem__(self, key: str) -> str:
        try:
            return _options_ext_get(self._solver, key)
        except BadParamError as e:
            raise KeyError(key) from e

    def __setitem__(self, key: str, value) -> None:
        _options_ext_set(
            self._solver,
            key,
            value if isinstance(value, str) else str(value),
        )

    def __delitem__(self, key: str) -> None:
        raise TypeError("SWMM extended options cannot be deleted")

    def __iter__(self):
        # No enumeration entry-point exposed by the C API yet.
        return iter(())

    def __len__(self) -> int:
        return 0


def _options_get(solver, str key) -> str:
    cdef SWMM_Engine h = <SWMM_Engine><size_t>solver.handle
    cdef bytes b = key.encode('utf-8')
    cdef char buf[256]
    _check(swmm_options_get(h, b, buf, 256))
    return buf.decode('utf-8')


def _options_set(solver, str key, str value) -> None:
    cdef SWMM_Engine h = <SWMM_Engine><size_t>solver.handle
    cdef bytes b_key = key.encode('utf-8')
    cdef bytes b_val = value.encode('utf-8')
    _check(swmm_options_set(h, b_key, b_val))


def _options_ext_get(solver, str key) -> str:
    cdef SWMM_Engine h = <SWMM_Engine><size_t>solver.handle
    cdef bytes b = key.encode('utf-8')
    cdef char buf[256]
    _check(swmm_options_get_ext(h, b, buf, 256))
    return buf.decode('utf-8')


def _options_ext_set(solver, str key, str value) -> None:
    cdef SWMM_Engine h = <SWMM_Engine><size_t>solver.handle
    cdef bytes b_key = key.encode('utf-8')
    cdef bytes b_val = value.encode('utf-8')
    _check(swmm_options_set_ext(h, b_key, b_val))


class UserFlags(MutableMapping):
    """``solver.userflags`` — typed name-keyed user-flag access.

    Reading the flag tries ``bool → int → real`` in order so callers see a
    Python value of the right native type. Writing chooses the matching
    C setter based on ``type(value)``.

    ``del solver.userflags[name]`` removes the flag's schema definition and
    every per-object value assigned to it (``swmm_userflag_undefine``).

    Beyond the mapping interface, the view exposes the ``[USER_FLAGS]``
    schema (:meth:`define`, :meth:`undefine`, :meth:`definitions`) and the
    ``[USER_FLAG_VALUES]`` per-object values (:meth:`get_value`,
    :meth:`set_value`, :meth:`clear_value`).
    """

    def __init__(self, solver):
        self._solver = solver

    def __getitem__(self, key):
        if not isinstance(key, str):
            raise TypeError(f"user flag name must be str, got {type(key).__name__}")
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = (<str>key).encode('utf-8')
        cdef int iv = 0
        cdef double dv = 0.0
        cdef char sbuf[512]
        cdef int found = 0
        # Try bool first, then int, then real, then string (the scalar
        # store is the MODEL-scoped per-object value). If all four fail
        # the key isn't a user flag at all.
        cdef int rc = swmm_userflag_get_bool(h, b, &iv)
        if rc == 0:
            return bool(iv)
        rc = swmm_userflag_get_int(h, b, &iv)
        if rc == 0:
            return int(iv)
        rc = swmm_userflag_get_real(h, b, &dv)
        if rc == 0:
            return float(dv)
        rc = swmm_userflag_value_get(h, b"MODEL", b"", b, sbuf, 512, &found)
        if rc == 0 and found:
            return sbuf.decode('utf-8')
        raise KeyError(key)

    def __setitem__(self, key, value):
        if not isinstance(key, str):
            raise TypeError(f"user flag name must be str, got {type(key).__name__}")
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = (<str>key).encode('utf-8')
        cdef bytes b_val
        if isinstance(value, bool):
            _check(swmm_userflag_set_bool(h, b, 1 if value else 0))
        elif isinstance(value, int):
            _check(swmm_userflag_set_int(h, b, <int>value))
        elif isinstance(value, float):
            _check(swmm_userflag_set_real(h, b, <double>value))
        elif isinstance(value, str):
            # Mirror the typed setters: auto-define as STRING, then store
            # the MODEL-scoped value.
            _check(swmm_userflag_define(h, b, 3, b""))
            b_val = (<str>value).encode('utf-8')
            _check(swmm_userflag_value_set(h, b"MODEL", b"", b, b_val))
        else:
            raise TypeError(
                "user flag value must be bool, int, float, or str; got "
                + type(value).__name__
            )

    def __delitem__(self, key):
        if not isinstance(key, str):
            raise TypeError(f"user flag name must be str, got {type(key).__name__}")
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = (<str>key).encode('utf-8')
        if swmm_userflag_undefine(h, b) != 0:
            raise KeyError(key)

    def __iter__(self):
        return iter([d.name for d in self.definitions()])

    def __len__(self) -> int:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_userflag_def_count(h, &v))
        return v

    def __contains__(self, key) -> bool:
        try:
            self[key]
            return True
        except KeyError:
            return False
        except TypeError:
            return False

    # -- [USER_FLAGS] schema definitions -----------------------------------

    def define(self, str name, int type, str description="") -> None:
        """Define (or redefine) a user flag (C{[USER_FLAGS]} schema).

        Redefining an existing name overwrites its definition; previously
        assigned per-object values are kept as-is.

        @param name: Flag name (stored uppercase).
        @type name: str
        @param type: Flag type: 0=BOOLEAN, 1=INTEGER, 2=REAL, 3=STRING
            (see L{UserFlagType}).
        @type type: int
        @param description: Optional description.
        @type description: str
        @return: None
        @rtype: None
        @raise EngineError: On empty name or invalid type.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b_name = name.encode('utf-8')
        cdef bytes b_desc = description.encode('utf-8')
        _check(swmm_userflag_define(h, b_name, type, b_desc))

    def undefine(self, str name) -> None:
        """Remove a flag definition and all its per-object values.

        @param name: Flag name (case-insensitive).
        @type name: str
        @return: None
        @rtype: None
        @raise EngineError: If the flag is not defined.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = name.encode('utf-8')
        _check(swmm_userflag_undefine(h, b))

    def definitions(self) -> list:
        """Return every user-flag schema definition, in insertion order.

        @return: List of L{UserFlagDef} named tuples
            C{(name, type, description)}.
        @rtype: list[UserFlagDef]
        @raise EngineError: On C API failure.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = 0
        cdef int t = 0
        cdef char name_buf[128]
        cdef char desc_buf[512]
        _check(swmm_userflag_def_count(h, &n))
        out = []
        for i in range(n):
            _check(swmm_userflag_def_get(h, i, name_buf, 128, &t,
                                         desc_buf, 512))
            out.append(UserFlagDef(name_buf.decode('utf-8'), t,
                                   desc_buf.decode('utf-8')))
        return out

    # -- [USER_FLAG_VALUES] per-object values -------------------------------

    def get_value(self, str obj_type, str obj_name, str flag_name):
        """Return the flag value assigned to a specific object, as a string.

        String form is symmetric with the INP encoding: BOOLEAN as
        C{YES}/C{NO}, INTEGER as a decimal, REAL as C{%g}, STRING verbatim.

        @param obj_type: Object type token (e.g. C{"NODE"}, C{"LINK"},
            C{"SUBCATCHMENT"}); case-insensitive.
        @type obj_type: str
        @param obj_name: Object identifier (case-preserved).
        @type obj_name: str
        @param flag_name: Flag name (case-insensitive).
        @type flag_name: str
        @return: The value string, or C{None} when no value is assigned.
        @rtype: str or None
        @raise EngineError: On C API failure.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b_type = obj_type.encode('utf-8')
        cdef bytes b_name = obj_name.encode('utf-8')
        cdef bytes b_flag = flag_name.encode('utf-8')
        cdef char buf[512]
        cdef int found = 0
        _check(swmm_userflag_value_get(h, b_type, b_name, b_flag,
                                       buf, 512, &found))
        if not found:
            return None
        return buf.decode('utf-8')

    def set_value(self, str obj_type, str obj_name, str flag_name,
                  str value) -> None:
        """Assign a flag value to a specific object from a string.

        The flag must already be defined (its declared type drives parsing).
        BOOLEAN accepts C{YES}/C{NO}/C{TRUE}/C{FALSE}/C{1}/C{0}; INTEGER a
        decimal integer; REAL a decimal number; STRING is stored verbatim.

        @param obj_type: Object type token; case-insensitive.
        @type obj_type: str
        @param obj_name: Object identifier (case-preserved).
        @type obj_name: str
        @param flag_name: Flag name (case-insensitive); must be defined.
        @type flag_name: str
        @param value: Value string parsed per the flag's declared type.
        @type value: str
        @return: None
        @rtype: None
        @raise EngineError: On undefined flag or a value that does not
            parse as the declared type.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b_type = obj_type.encode('utf-8')
        cdef bytes b_name = obj_name.encode('utf-8')
        cdef bytes b_flag = flag_name.encode('utf-8')
        cdef bytes b_val = value.encode('utf-8')
        _check(swmm_userflag_value_set(h, b_type, b_name, b_flag, b_val))

    def clear_value(self, str obj_type, str obj_name, str flag_name) -> None:
        """Remove the flag value assigned to a specific object (mark unset).

        Clearing an unassigned value succeeds (idempotent).

        @param obj_type: Object type token; case-insensitive.
        @type obj_type: str
        @param obj_name: Object identifier (case-preserved).
        @type obj_name: str
        @param flag_name: Flag name (case-insensitive).
        @type flag_name: str
        @return: None
        @rtype: None
        @raise EngineError: On C API failure.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b_type = obj_type.encode('utf-8')
        cdef bytes b_name = obj_name.encode('utf-8')
        cdef bytes b_flag = flag_name.encode('utf-8')
        _check(swmm_userflag_value_clear(h, b_type, b_name, b_flag))


class EventsView(MutableSequence):
    """``solver.events`` — ``MutableSequence[Event]`` for the ``[EVENTS]`` block.

    Each entry is an :class:`Event` named tuple with ``.start`` and ``.end``
    attributes typed as :class:`datetime.datetime`.

    .. code-block:: python

        solver.events.append(start=datetime(2024,1,1), end=datetime(2024,1,2))
        for ev in solver.events:
            print(ev.start, ev.end)
        del solver.events[0]
        solver.events.clear()
    """

    def __init__(self, solver):
        self._solver = solver

    def _count(self) -> int:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_events_count(h, &v))
        return v

    def __len__(self) -> int:
        return self._count()

    def __getitem__(self, idx):
        if isinstance(idx, slice):
            return [self[i] for i in range(*idx.indices(len(self)))]
        if not isinstance(idx, int):
            raise TypeError(
                "Event index must be int or slice, got " + type(idx).__name__
            )
        n = len(self)
        if idx < 0:
            idx += n
        if idx < 0 or idx >= n:
            raise IndexError(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double s = 0.0
        cdef double e = 0.0
        _check(swmm_events_get(h, idx, &s, &e))
        return Event(oadate_to_datetime(s), oadate_to_datetime(e))

    def __setitem__(self, idx, value):
        if not isinstance(idx, int):
            raise TypeError("Event index must be int")
        n = len(self)
        if idx < 0:
            idx += n
        if idx < 0 or idx >= n:
            raise IndexError(idx)
        start, end = self._unpack(value)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_events_set(
            h, idx, datetime_to_oadate(start), datetime_to_oadate(end)))

    def __delitem__(self, idx):
        if not isinstance(idx, int):
            raise TypeError("Event index must be int")
        n = len(self)
        if idx < 0:
            idx += n
        if idx < 0 or idx >= n:
            raise IndexError(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_events_remove(h, idx))

    def insert(self, idx, value):
        """Insert *value* at *idx* (emulated via clear + re-add; the C event API is append-only)."""
        # The C API only supports append; emulate insert by clearing and
        # re-adding. For ``idx >= len(self)`` this degenerates to append.
        n = len(self)
        if idx < 0:
            idx += n
        idx = max(0, min(idx, n))
        existing = [self[i] for i in range(n)]
        existing.insert(idx, value)
        self.clear()
        for ev in existing:
            self.append(ev)

    def append(self, value):
        """Append a reporting / hot-start event *value*."""
        start, end = self._unpack(value)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int new_idx = -1
        _check(swmm_events_add(
            h, datetime_to_oadate(start), datetime_to_oadate(end), &new_idx))

    def clear(self):
        """Remove all scheduled events."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_events_clear(h))

    @staticmethod
    def _unpack(value):
        """Accept Event(start, end), (start, end), or {'start':..., 'end':...}."""
        if isinstance(value, Event):
            return value.start, value.end
        if isinstance(value, dict):
            return value["start"], value["end"]
        try:
            s, e = value
            return s, e
        except (TypeError, ValueError) as exc:
            raise TypeError(
                "Event entry must be an Event, (start, end) tuple, or "
                "dict with 'start'/'end' keys"
            ) from exc

    def __repr__(self) -> str:
        try:
            return f"<EventsView n={len(self)}>"
        except EngineError:
            return "<EventsView (engine closed)>"


# =============================================================================
# Module-level convenience functions — raise on failure now (no int rc).
# =============================================================================

def run(inp, rpt=None, out=None, *, plugin_lib=None) -> None:
    """Run a SWMM simulation from start to finish in a single call.

    :raises EngineError: On any non-zero C return.
    """
    cdef bytes b_inp = _path_to_str(inp).encode('utf-8')
    cdef bytes b_rpt = _path_to_str(rpt).encode('utf-8')
    cdef bytes b_out = _path_to_str(out).encode('utf-8')
    cdef bytes b_plugin
    cdef const char* c_plugin = NULL
    pl = _path_to_str(plugin_lib) if plugin_lib else None
    if pl:
        b_plugin = pl.encode('utf-8')
        c_plugin = b_plugin
    _check(swmm_engine_run(b_inp, b_rpt, b_out, c_plugin))


def run_with_callback(inp, rpt=None, out=None,
                      callback=None, *, plugin_lib=None) -> None:
    """Run a SWMM simulation with an optional progress callback.

    :param callback: A callable receiving ``(progress: float)`` where
        progress is in ``[0, 1]``. ``None`` to disable.
    :raises EngineError: On any non-zero C return.
    """
    cdef bytes b_inp = _path_to_str(inp).encode('utf-8')
    cdef bytes b_rpt = _path_to_str(rpt).encode('utf-8')
    cdef bytes b_out = _path_to_str(out).encode('utf-8')
    cdef bytes b_plugin
    cdef const char* c_plugin = NULL
    pl = _path_to_str(plugin_lib) if plugin_lib else None
    if pl:
        b_plugin = pl.encode('utf-8')
        c_plugin = b_plugin
    if callback is None:
        _check(swmm_engine_run(b_inp, b_rpt, b_out, c_plugin))
        return
    _check(swmm_engine_run_with_callback(
        b_inp, b_rpt, b_out, c_plugin, _progress_trampoline, <void*>callback))
