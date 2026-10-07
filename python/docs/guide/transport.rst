Transport configuration and availability
========================================

``solver.transport`` exposes the configured ARD transport options. Dispersion
coefficients use project length squared per second (ft²/s or m²/s), and
``target_dx`` uses project length. Set both mode and coefficient for a uniform
coefficient::

    from openswmm.engine import Solver, TransportDispersionMode

    solver = Solver("model.inp")
    try:
        solver.open()
        solver.options["QUALITY_SOLVER"] = "EULERIAN_ARD"
        component = "org.hydrocouple.openswmm.transport.ard"
        if component not in solver.process_components:
            solver.process_components.register(component, "model.ard")
        solver.transport.dispersion_mode = TransportDispersionMode.VALUE
        solver.transport.dispersion_value = 0.5
        solver.transport.target_dx = 10.0
        solver.initialize()
        solver.start()
        for elapsed in solver.steps():
            pass
        solver.end()
    finally:
        solver.close()
        solver.destroy()

For persistence, the model must register
``org.hydrocouple.openswmm.transport.ard`` with a configuration-file path.
``solver.write()`` then renders the live configuration to that component file.
Setting values alone does not add a process-component registration.

Configure transport before initialization. Setters mark the configuration as
present; this does not establish that a particular backend carries every
species in every domain. Inspect ``solver.transport_matrix`` after opening or
editing options. It maps ``TransportDomain`` to ``TransportClass`` to immutable
``TransportCell(state, count, reason)`` records. Disabled or unavailable cells
explain why the process is absent.

``transport.boundaries``, ``transport.sources`` and ``transport.conduit_dispersion``
are tuples of snapshot records. Boundary/source rows are read-only in this
native API revision; author them in the component file before opening. For a
VALUE row, a boundary value is concentration in species units, and a source
value is a mass rate in species mass units per second. For a TIMESERIES row,
use the ``timeseries`` name instead of the numeric value.

``solver.thread_info`` reports process-wide CPU and OpenMP limits.
``solver.effective_threads(requested=0)`` reports the team sizes the engine
would select for the current model; zero requests automatic selection. This
query does not start a run or change the ``THREADS`` option.

.. automodule:: openswmm.engine._transport
   :members:
