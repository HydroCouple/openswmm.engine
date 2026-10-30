Engine ownership and callbacks
==============================

A ``Solver`` owns its native engine. Its domain views retain that owner, so a
view cannot outlive the Python object that manages its native memory. Calling
``close()`` or ``destroy()`` invalidates retained 2D and infiltration views;
structural edits also invalidate these views. Reacquire ``solver.surface2d``
after reopening or editing a model. Invalidated views raise
``StaleObjectError`` before entering the native library.

Use ``solver.surface2d`` or ``Surface2D(solver)``. Passing ``solver.handle`` to
``Surface2D`` is deprecated. For compatibility it is accepted only when the
address belongs to a live registered Python owner. Arbitrary native addresses
are rejected with ``BadHandleError``.

``ModelBuilder.to_solver()`` transfers ownership once. Existing builder views
become stale, and a second transfer raises ``BadHandleError``. The returned
solver has the same initialized Python state as a normally constructed solver.

Threads
-------

Different solvers may advance on different threads. Calls that release the
Python GIL pin their engine owner for the duration of the native operation.
Access from another thread to that same engine raises ``LifecycleError``
promptly. This includes attempts to destroy the engine while it is advancing.
Coordinate access in the application and collect snapshots between steps.

Callbacks
---------

Step callbacks run on the advancing thread. They may inspect results and apply
forcing supported at that stage by the engine. They must not reenter lifecycle
operations such as ``step()``, ``close()`` or ``destroy()``, or replace callback
registrations while a callback is active; these operations raise
``LifecycleError``.

A Python exception cannot unwind through the native callback ABI. The binding
captures the first exception and re-raises that same exception when the native
operation returns. Later callbacks in that operation are suppressed. The
current native step may finish, so callback failure does not roll back the
simulation. This behavior applies to ``step()``, ``stride()``, ``steps()`` and
``until()``. Unregister or repair the failing callback before advancing again.

Testing a local build
---------------------

Use the project's ``openswmm`` Conda environment for validation::

    conda run --no-capture-output -n openswmm python -m pytest tests/engine

Run this command from ``python/`` after building/installing the checkout into
that environment, or set ``PYTHONPATH`` to a staged build. Verify the loaded
extension path before testing::

    conda run -n openswmm python -c "import openswmm.engine._solver as m; print(m.__file__)"

A successful test of an older installed package does not validate a changed
checkout. Rebuild all extensions after changing a shared ``.pxd`` declaration.
