"""
Engine Lifecycle
================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

The :class:`Solver` class manages the full SWMM engine lifecycle:
create -> open -> initialize -> start -> step -> end -> report -> close -> destroy.

It is the primary entry point for running a simulation from Python.

Usage as a context manager
--------------------------

.. code-block:: python

    from openswmm.engine import Solver, Nodes, Links, EngineState

    with Solver("model.inp", "model.rpt", "model.out") as s:
        nodes = Nodes(s)
        links = Links(s)
        while s.state == EngineState.RUNNING:
            rc = s.step()
            if rc != 0:
                break
            depth = nodes.get_depth("J1")
            flow  = links.get_flow("C1")
            print(f"Elapsed: {s.elapsed:.4f} days")

Manual lifecycle
----------------

.. code-block:: python

    from openswmm.engine import Solver, EngineState

    s = Solver("model.inp")
    rc = s.open() or s.initialize() or s.start()
    if rc != 0:
        raise RuntimeError(f"open/initialize/start failed: rc={rc}")
    while s.state == EngineState.RUNNING:
        if s.step() != 0:
            break
    s.end()
    s.report()
    s.close()
    s.destroy()
"""

# cython: language_level=3

from datetime import datetime

from ._common cimport *
from ._dates import datetime_to_oadate, oadate_to_datetime


class EngineError(Exception):
    """Raised when a C API call returns a non-zero error code.

    @ivar code: The SWMM error code returned by the C API.
    @ivar message: Human-readable error description.

    @param code: The SWMM error code.
    @type code: int
    @param message: Human-readable error description. When empty, the
        message is derived from C{code} via the SWMM C API helper
        C{swmm_error_message}.
    @type message: str
    """

    def __init__(self, int code, str message=""):
        cdef const char* msg
        self.code = code
        if not message:
            msg = swmm_error_message(code)
            message = msg.decode('utf-8') if msg != NULL else f"Error {code}"
        self.message = message
        super().__init__(self.message)


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


cdef class Solver:
    """SWMM engine lifecycle manager.

    Manages the complete SWMM simulation lifecycle from file parsing through
    timestep execution to report generation. Supports both manual lifecycle
    control and the context manager protocol.

    @param inp: Path to the SWMM input file (C{.inp}).
    @type inp: str
    @param rpt: Path for the report file (C{.rpt}). Empty string to skip.
    @type rpt: str
    @param out: Path for the binary output file (C{.out}). Empty string to skip.
    @type out: str

    @note: The engine handle is created lazily on the first call to L{open}.
        Multiple independent L{Solver} instances may coexist.

    Example::

        with Solver("model.inp", "model.rpt", "model.out") as s:
            while s.state == EngineState.RUNNING:
                if s.step() != 0:
                    break
                print(f"Elapsed: {s.elapsed:.4f} days")
    """

    def __init__(self, str inp="", str rpt="", str out=""):
        self._inp = inp
        self._rpt = rpt
        self._out = out
        self._elapsed = 0.0
        self._handle = NULL
        self._step_begin_cb = None
        self._step_end_cb = None

    # =========================================================================
    # Lifecycle
    # =========================================================================

    def create(self):
        """Create the engine instance.

        Allocates the underlying C engine handle. Normally called automatically
        by L{open}; explicit calls are only needed when working with the
        engine handle directly.

        @return: None
        @rtype: None
        @raise MemoryError: If allocation fails.
        """
        self._handle = swmm_engine_create()
        if self._handle == NULL:
            raise MemoryError("Failed to create engine")

    def open(self, str plugin_lib=None) -> int:
        """Open and parse the input file; load plugins.

        Transitions the engine to C{OPENED} state.

        @param plugin_lib: Optional path to a plugin shared library. When
            C{None}, no plugin library is loaded.
        @type plugin_lib: str or None
        @return: Error code from the C API (C{0} on success, non-zero on
            failure). Lifecycle status is queryable via L{state}.
        @rtype: int
        """
        if self._handle == NULL:
            self.create()
        cdef bytes b_inp = self._inp.encode('utf-8')
        cdef bytes b_rpt = self._rpt.encode('utf-8')
        cdef bytes b_out = self._out.encode('utf-8')
        cdef bytes b_plugin
        cdef const char* c_plugin = NULL
        if plugin_lib is not None:
            b_plugin = plugin_lib.encode('utf-8')
            c_plugin = b_plugin
        return swmm_engine_open(self._handle, b_inp, b_rpt, b_out, c_plugin)

    def initialize(self) -> int:
        """Initialize the simulation.

        Allocates simulation arrays and applies initial conditions.
        Transitions the engine to C{INITIALIZED} state.

        @return: Error code from the C API (C{0} on success, non-zero on
            failure).
        @rtype: int
        """
        return swmm_engine_initialize(self._handle)

    def start(self, bint save_results=True) -> int:
        """Start the simulation.

        Transitions the engine to C{RUNNING} state and prepares the binary
        output file when C{save_results} is C{True}.

        @param save_results: If C{True}, write binary output to the C{.out}
            file. When C{False}, no binary output is produced.
        @type save_results: bool
        @return: Error code from the C API (C{0} on success, non-zero on
            failure).
        @rtype: int
        """
        return swmm_engine_start(self._handle, 1 if save_results else 0)

    def step(self) -> int:
        """Advance the simulation by one explicit timestep.

        Caller polls L{state} to detect completion: when the simulation is
        finished the engine transitions from C{RUNNING} to C{ENDED}, so the
        idiomatic loop is::

            from openswmm.engine import EngineState
            while solver.state == EngineState.RUNNING:
                rc = solver.step()
                if rc != 0:
                    break

        Updates L{elapsed} as a side effect.

        @return: Error code from the C API (C{0} on success, non-zero on
            failure).
        @rtype: int
        """
        cdef double elapsed = 0.0
        cdef int rc = swmm_engine_step(self._handle, &elapsed)
        self._elapsed = elapsed
        return rc

    def stride(self, int n_steps) -> int:
        """Advance the simulation by C{n_steps} timesteps in one call.

        Updates L{elapsed} as a side effect.

        @param n_steps: Number of timesteps to advance.
        @type n_steps: int
        @return: Error code from the C API (C{0} on success, non-zero on
            failure).
        @rtype: int
        """
        cdef double elapsed = 0.0
        cdef int rc = swmm_engine_stride(self._handle, n_steps, &elapsed)
        self._elapsed = elapsed
        return rc

    def end(self) -> int:
        """End the simulation; join the IO thread; finalize plugins.

        Transitions the engine to C{ENDED} state.

        @return: Error code from the C API (C{0} on success, non-zero on
            failure).
        @rtype: int
        """
        return swmm_engine_end(self._handle)

    def report(self) -> int:
        """Write the summary report to the C{.rpt} file.

        @return: Error code from the C API (C{0} on success, non-zero on
            failure).
        @rtype: int
        """
        return swmm_engine_report(self._handle)

    def close(self) -> int:
        """Close all files and free simulation state.

        Transitions the engine to C{CLOSED} state. Safe to call when the
        handle is already C{NULL}.

        @return: Error code from the C API (C{0} on success or when the
            handle is C{NULL}, non-zero on failure).
        @rtype: int
        """
        if self._handle == NULL:
            return 0
        return swmm_engine_close(self._handle)

    def destroy(self):
        """Destroy the engine handle and free all memory.

        After this call the L{Solver} no longer holds a valid C handle.
        Safe to call when the handle is already C{NULL}.

        @return: None
        @rtype: None
        """
        if self._handle != NULL:
            swmm_engine_destroy(self._handle)
            self._handle = NULL

    # =========================================================================
    # Timing properties
    # =========================================================================

    @property
    def elapsed(self) -> float:
        """Elapsed simulation time in decimal days after the last L{step}.

        @return: Elapsed simulation time (decimal days).
        @rtype: float
        """
        return self._elapsed

    # =========================================================================
    # State / handle
    # =========================================================================

    @property
    def state(self) -> int:
        """Current engine lifecycle state code.

        @return: Lifecycle state code.
        @rtype: int
        @see: L{openswmm.engine._enums.EngineState} for code meanings.
        """
        cdef int s = 0
        _check(swmm_engine_get_state(self._handle, &s))
        return s

    @property
    def handle(self):
        """Raw C engine handle (for advanced interop).

        @return: The underlying C engine pointer cast to an integer.
        @rtype: int
        """
        return <size_t>self._handle

    # =========================================================================
    # Timing accessors
    # =========================================================================

    def get_start_time(self) -> float:
        """Return the simulation start time.

        @return: Simulation start time (decimal days).
        @rtype: float
        @raise EngineError: On C API failure.
        """
        cdef double v = 0.0
        _check(swmm_get_start_time(self._handle, &v))
        return v

    def get_end_time(self) -> float:
        """Return the simulation end time.

        @return: Simulation end time (decimal days).
        @rtype: float
        @raise EngineError: On C API failure.
        """
        cdef double v = 0.0
        _check(swmm_get_end_time(self._handle, &v))
        return v

    def get_current_time(self) -> float:
        """Return the current simulation time.

        @return: Current simulation time (decimal days).
        @rtype: float
        @raise EngineError: On C API failure.
        """
        cdef double v = 0.0
        _check(swmm_get_current_time(self._handle, &v))
        return v

    def get_routing_step(self) -> float:
        """Return the routing timestep.

        @return: Routing timestep (seconds).
        @rtype: float
        @raise EngineError: On C API failure.
        """
        cdef double v = 0.0
        _check(swmm_get_routing_step(self._handle, &v))
        return v

    # =========================================================================
    # Model write
    # =========================================================================

    def model_write(self, str path):
        """Write the current model to a SWMM C{.inp} file.

        @param path: Output file path.
        @type path: str
        @return: None
        @rtype: None
        @raise EngineError: On C API failure.
        """
        cdef bytes b = path.encode('utf-8')
        _check(swmm_model_write(self._handle, b))

    # =========================================================================
    # Options
    # =========================================================================

    def get_option(self, str key) -> str:
        """Return the value of a model option.

        @param key: Option key name.
        @type key: str
        @return: Option value as a string.
        @rtype: str
        @raise EngineError: On C API failure.
        """
        cdef bytes b = key.encode('utf-8')
        cdef char buf[256]
        _check(swmm_options_get(self._handle, b, buf, 256))
        return buf.decode('utf-8')

    def set_option(self, str key, str value):
        """Set a model option.

        @param key: Option key name.
        @type key: str
        @param value: Option value string.
        @type value: str
        @return: None
        @rtype: None
        @raise EngineError: On C API failure.
        """
        cdef bytes b_key = key.encode('utf-8')
        cdef bytes b_val = value.encode('utf-8')
        _check(swmm_options_set(self._handle, b_key, b_val))

    def get_option_ext(self, str key) -> str:
        """Return the value of an extended model option.

        @param key: Extended option key name.
        @type key: str
        @return: Extended option value as a string.
        @rtype: str
        @raise EngineError: On C API failure.
        """
        cdef bytes b = key.encode('utf-8')
        cdef char buf[256]
        _check(swmm_options_get_ext(self._handle, b, buf, 256))
        return buf.decode('utf-8')

    def set_option_ext(self, str key, str value):
        """Set an extended model option.

        @param key: Extended option key name.
        @type key: str
        @param value: Extended option value string.
        @type value: str
        @return: None
        @rtype: None
        @raise EngineError: On C API failure.
        """
        cdef bytes b_key = key.encode('utf-8')
        cdef bytes b_val = value.encode('utf-8')
        _check(swmm_options_set_ext(self._handle, b_key, b_val))

    def get_crs(self) -> str:
        """Return the coordinate reference system string.

        @return: CRS string (e.g. EPSG identifier or WKT) for the current
            model.
        @rtype: str
        @raise EngineError: On C API failure.
        """
        cdef char buf[256]
        _check(swmm_get_crs(self._handle, buf, 256))
        return buf.decode('utf-8')

    # =========================================================================
    # Typed time-control properties (datetime)
    # =========================================================================

    @property
    def start_datetime(self) -> datetime:
        """Simulation start date/time.

        @rtype: datetime.datetime
        @raise EngineError: On C API failure.
        """
        cdef double v = 0.0
        _check(swmm_options_get_start_date(self._handle, &v))
        return oadate_to_datetime(v)

    @start_datetime.setter
    def start_datetime(self, value: datetime) -> None:
        cdef double v = datetime_to_oadate(value)
        _check(swmm_options_set_start_date(self._handle, v))

    @property
    def end_datetime(self) -> datetime:
        """Simulation end date/time.

        @rtype: datetime.datetime
        @raise EngineError: On C API failure.
        """
        cdef double v = 0.0
        _check(swmm_options_get_end_date(self._handle, &v))
        return oadate_to_datetime(v)

    @end_datetime.setter
    def end_datetime(self, value: datetime) -> None:
        cdef double v = datetime_to_oadate(value)
        _check(swmm_options_set_end_date(self._handle, v))

    @property
    def report_start_datetime(self) -> datetime:
        """Report start date/time.

        @rtype: datetime.datetime
        @raise EngineError: On C API failure.
        """
        cdef double v = 0.0
        _check(swmm_options_get_report_start(self._handle, &v))
        return oadate_to_datetime(v)

    @report_start_datetime.setter
    def report_start_datetime(self, value: datetime) -> None:
        cdef double v = datetime_to_oadate(value)
        _check(swmm_options_set_report_start(self._handle, v))

    # =========================================================================
    # Routing events / steady-state
    # =========================================================================

    def is_between_events(self) -> bool:
        """Check whether the simulation is currently between routing events.

        @return: C{True} if between events (routing skipped); C{False}
            otherwise.
        @rtype: bool
        @raise EngineError: On C API failure.
        """
        cdef int v = 0
        _check(swmm_is_between_events(self._handle, &v))
        return v != 0

    def get_event_count(self) -> int:
        """Get the number of routing events defined in the C{[EVENTS]} section.

        @return: Event count.
        @rtype: int
        @raise EngineError: On C API failure.
        """
        cdef int v = 0
        _check(swmm_get_event_count(self._handle, &v))
        return v

    def get_steady_state_skip(self) -> bool:
        """Check whether steady-state routing skip is enabled.

        @return: C{True} if C{SKIP_STEADY_STATE} is enabled.
        @rtype: bool
        @raise EngineError: On C API failure.
        """
        cdef int v = 0
        _check(swmm_get_steady_state_skip(self._handle, &v))
        return v != 0

    def set_steady_state_skip(self, bint enabled):
        """Enable or disable steady-state routing skip.

        @param enabled: C{True} to enable, C{False} to disable.
        @type enabled: bool
        @return: None
        @rtype: None
        @raise EngineError: On C API failure.
        """
        _check(swmm_set_steady_state_skip(self._handle, 1 if enabled else 0))

    # =========================================================================
    # Step callbacks
    # =========================================================================

    def set_step_begin_callback(self, callback):
        """Register a callback invoked at the start of each timestep.

        The callable receives C{(sim_time: float, dt: float)} and is invoked
        before physics are computed -- the right place to inject forcings or
        modify boundary conditions. Pass C{None} to unregister.

        @param callback: A Python callable accepting C{(sim_time, dt)} or
            C{None} to unregister.
        @type callback: callable or None
        @return: None
        @rtype: None
        @raise EngineError: On C API failure.
        """
        if callback is None:
            self._step_begin_cb = None
            _check(swmm_set_step_begin_callback(self._handle, NULL, NULL))
        else:
            self._step_begin_cb = callback
            _check(swmm_set_step_begin_callback(
                self._handle, _step_begin_trampoline,
                <void*>self._step_begin_cb))

    def set_step_end_callback(self, callback):
        """Register a callback invoked at the end of each timestep.

        The callable receives C{(sim_time: float, dt: float)} and is invoked
        after physics -- the right place to read results for real-time
        monitoring or control. Pass C{None} to unregister.

        @param callback: A Python callable accepting C{(sim_time, dt)} or
            C{None} to unregister.
        @type callback: callable or None
        @return: None
        @rtype: None
        @raise EngineError: On C API failure.
        """
        if callback is None:
            self._step_end_cb = None
            _check(swmm_set_step_end_callback(self._handle, NULL, NULL))
        else:
            self._step_end_cb = callback
            _check(swmm_set_step_end_callback(
                self._handle, _step_end_trampoline,
                <void*>self._step_end_cb))

    # =========================================================================
    # Context manager
    # =========================================================================

    def __enter__(self):
        cdef int rc = self.open()
        if rc != 0:
            raise EngineError(rc)
        rc = self.initialize()
        if rc != 0:
            raise EngineError(rc)
        rc = self.start()
        if rc != 0:
            raise EngineError(rc)
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        # Lifecycle teardown ignores per-step rc -- destroy() always runs.
        self.end()
        self.report()
        self.close()
        self.destroy()
        return False


# =============================================================================
# Module-level convenience functions
# =============================================================================

def run(str inp, str rpt="", str out="", str plugin_lib=None) -> int:
    """Run a SWMM simulation from start to finish in a single call.

    Convenience wrapper that performs the full lifecycle (open through
    destroy) without exposing intermediate state.

    @param inp: Path to the SWMM input file (C{.inp}).
    @type inp: str
    @param rpt: Path for the report file (C{.rpt}). Empty string to skip.
    @type rpt: str
    @param out: Path for the binary output file (C{.out}). Empty string to
        skip.
    @type out: str
    @param plugin_lib: Optional path to a plugin shared library. When
        C{None}, no plugin library is loaded.
    @type plugin_lib: str or None
    @return: Error code from the C API (C{0} on success, non-zero on
        failure).
    @rtype: int
    """
    cdef bytes b_inp = inp.encode('utf-8')
    cdef bytes b_rpt = rpt.encode('utf-8')
    cdef bytes b_out = out.encode('utf-8')
    cdef bytes b_plugin
    cdef const char* c_plugin = NULL
    if plugin_lib is not None:
        b_plugin = plugin_lib.encode('utf-8')
        c_plugin = b_plugin
    return swmm_engine_run(b_inp, b_rpt, b_out, c_plugin)


def run_with_callback(str inp, str rpt="", str out="",
                      callback=None, str plugin_lib=None) -> int:
    """Run a SWMM simulation with a progress callback.

    Convenience wrapper around L{run} that periodically invokes C{callback}
    with the simulation's elapsed-time fraction. When C{callback} is C{None},
    behaves identically to L{run}.

    @param inp: Path to the SWMM input file (C{.inp}).
    @type inp: str
    @param rpt: Path for the report file (C{.rpt}). Empty string to skip.
    @type rpt: str
    @param out: Path for the binary output file (C{.out}). Empty string to
        skip.
    @type out: str
    @param callback: A callable receiving C{(progress: float)} where
        C{progress} is a fraction in C{[0, 1]}. C{None} disables the
        callback.
    @type callback: callable or None
    @param plugin_lib: Optional path to a plugin shared library.
    @type plugin_lib: str or None
    @return: Error code from the C API (C{0} on success, non-zero on
        failure).
    @rtype: int
    """
    cdef bytes b_inp = inp.encode('utf-8')
    cdef bytes b_rpt = rpt.encode('utf-8')
    cdef bytes b_out = out.encode('utf-8')
    cdef bytes b_plugin
    cdef const char* c_plugin = NULL
    if plugin_lib is not None:
        b_plugin = plugin_lib.encode('utf-8')
        c_plugin = b_plugin
    if callback is None:
        return swmm_engine_run(b_inp, b_rpt, b_out, c_plugin)
    return swmm_engine_run_with_callback(
        b_inp, b_rpt, b_out, c_plugin,
        _progress_trampoline, <void*>callback)
