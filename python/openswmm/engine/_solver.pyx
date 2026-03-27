"""
Engine Lifecycle
================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

The :class:`Solver` class manages the full SWMM engine lifecycle:
create -> open -> initialize -> start -> step -> end -> report -> close -> destroy.

It is the primary entry point for running a simulation from Python.

Usage as a context manager
--------------------------

.. code-block:: python

    from openswmm.engine import Solver, Nodes, Links

    with Solver("model.inp", "model.rpt", "model.out") as s:
        nodes = Nodes(s)
        links = Links(s)
        while s.step():
            depth = nodes.get_depth("J1")
            flow  = links.get_flow("C1")
            print(f"Elapsed: {s.elapsed:.4f} days")

Manual lifecycle
----------------

.. code-block:: python

    s = Solver("model.inp")
    s.open()
    s.initialize()
    s.start()
    while s.step():
        pass
    s.end()
    s.report()
    s.close()
    s.destroy()
"""

# cython: language_level=3

from ._common cimport *


class EngineError(Exception):
    """Raised when a C API call returns a non-zero error code.

    :param code: The SWMM error code.
    :type code: int
    :param message: Human-readable error description.
    :type message: str
    """

    def __init__(self, int code, str message=""):
        self.code = code
        if not message:
            cdef const char* msg = swmm_error_message(code)
            message = msg.decode('utf-8') if msg != NULL else f"Error {code}"
        self.message = message
        super().__init__(self.message)


cdef class Solver:
    """SWMM engine lifecycle manager.

    :param inp: Path to the SWMM input file (``.inp``).
    :type inp: str
    :param rpt: Path for the report file (``.rpt``). Empty string to skip.
    :type rpt: str
    :param out: Path for the binary output file (``.out``). Empty string to skip.
    :type out: str

    .. note::

       The engine handle is created lazily on the first call to :meth:`open`.
       Multiple independent :class:`Solver` instances may coexist.
    """

    def __init__(self, str inp="", str rpt="", str out=""):
        self._inp = inp
        self._rpt = rpt
        self._out = out
        self._elapsed = 0.0
        self._handle = NULL

    def create(self):
        """Create the engine instance.

        :raises MemoryError: If allocation fails.
        """
        self._handle = swmm_engine_create()
        if self._handle == NULL:
            raise MemoryError("Failed to create engine")

    def open(self):
        """Open and parse the input file; load plugins.

        Transitions the engine to ``OPENED`` state.

        :raises EngineError: If the input file cannot be parsed.
        """
        if self._handle == NULL:
            self.create()
        cdef bytes b_inp = self._inp.encode('utf-8')
        cdef bytes b_rpt = self._rpt.encode('utf-8')
        cdef bytes b_out = self._out.encode('utf-8')
        _check(swmm_engine_open(self._handle, b_inp, b_rpt, b_out))

    def initialize(self):
        """Initialize the simulation (allocate arrays, set initial conditions).

        Transitions the engine to ``INITIALIZED`` state.
        """
        _check(swmm_engine_initialize(self._handle))

    def start(self, bint save_results=True):
        """Start the simulation.

        :param save_results: If ``True``, write binary output to the ``.out`` file.
        :type save_results: bool
        """
        _check(swmm_engine_start(self._handle, 1 if save_results else 0))

    def step(self) -> bool:
        """Advance the simulation by one explicit timestep.

        :returns: ``True`` if the simulation should continue;
                  ``False`` when the simulation is complete.
        :rtype: bool
        """
        cdef double elapsed = 0.0
        _check(swmm_engine_step(self._handle, &elapsed))
        self._elapsed = elapsed
        return elapsed > 0.0

    def end(self):
        """End the simulation; join IO thread; finalize plugins."""
        _check(swmm_engine_end(self._handle))

    def report(self):
        """Write the summary report."""
        _check(swmm_engine_report(self._handle))

    def close(self):
        """Close all files and free simulation state."""
        if self._handle != NULL:
            _check(swmm_engine_close(self._handle))

    def destroy(self):
        """Destroy the engine handle and free all memory."""
        if self._handle != NULL:
            swmm_engine_destroy(self._handle)
            self._handle = NULL

    @property
    def elapsed(self) -> float:
        """Elapsed simulation time in decimal days after the last :meth:`step`."""
        return self._elapsed

    @property
    def state(self) -> int:
        """Current engine lifecycle state code.

        See :class:`~openswmm.engine._enums.EngineState` for code meanings.
        """
        cdef int s = 0
        _check(swmm_engine_get_state(self._handle, &s))
        return s

    @property
    def handle(self):
        """Raw C engine handle (for advanced interop)."""
        return <size_t>self._handle

    # --- Timing ---

    def get_start_time(self) -> float:
        """Return the simulation start time (decimal days)."""
        cdef double v = 0.0
        _check(swmm_get_start_time(self._handle, &v))
        return v

    def get_end_time(self) -> float:
        """Return the simulation end time (decimal days)."""
        cdef double v = 0.0
        _check(swmm_get_end_time(self._handle, &v))
        return v

    def get_current_time(self) -> float:
        """Return the current simulation time (decimal days)."""
        cdef double v = 0.0
        _check(swmm_get_current_time(self._handle, &v))
        return v

    def get_routing_step(self) -> float:
        """Return the routing timestep (seconds)."""
        cdef double v = 0.0
        _check(swmm_get_routing_step(self._handle, &v))
        return v

    # --- Model write ---

    def model_write(self, str path):
        """Write the current model to a SWMM ``.inp`` file.

        :param path: Output file path.
        :type path: str
        """
        cdef bytes b = path.encode('utf-8')
        _check(swmm_model_write(self._handle, b))

    # --- Options ---

    def get_option(self, str key) -> str:
        """Return the value of a model option.

        :param key: Option key name.
        :returns: Option value string.
        :rtype: str
        """
        cdef bytes b = key.encode('utf-8')
        cdef char buf[256]
        _check(swmm_options_get(self._handle, b, buf, 256))
        return buf.decode('utf-8')

    def set_option(self, str key, str value):
        """Set a model option.

        :param key: Option key name.
        :param value: Option value string.
        """
        cdef bytes b_key = key.encode('utf-8')
        cdef bytes b_val = value.encode('utf-8')
        _check(swmm_options_set(self._handle, b_key, b_val))

    def get_option_ext(self, str key) -> str:
        """Return the value of an extended model option.

        :param key: Extended option key name.
        :returns: Extended option value string.
        :rtype: str
        """
        cdef bytes b = key.encode('utf-8')
        cdef char buf[256]
        _check(swmm_options_get_ext(self._handle, b, buf, 256))
        return buf.decode('utf-8')

    def set_option_ext(self, str key, str value):
        """Set an extended model option.

        :param key: Extended option key name.
        :param value: Extended option value string.
        """
        cdef bytes b_key = key.encode('utf-8')
        cdef bytes b_val = value.encode('utf-8')
        _check(swmm_options_set_ext(self._handle, b_key, b_val))

    def get_crs(self) -> str:
        """Return the coordinate reference system string.

        :returns: CRS string.
        :rtype: str
        """
        cdef char buf[256]
        _check(swmm_get_crs(self._handle, buf, 256))
        return buf.decode('utf-8')

    # --- Context manager ---

    def __enter__(self):
        self.open()
        self.initialize()
        self.start()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        try:
            self.end()
            self.report()
        except Exception:
            pass
        try:
            self.close()
        except Exception:
            pass
        self.destroy()
        return False
