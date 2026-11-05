Native water peers and atomic editor records
============================================

Native water peers
------------------

``solver.split`` exposes the experimental native water-only peer profile.
It retains the native eligibility checks and lifecycle: configure the two
``OPENED`` engines before initialization, export the initialized surface
geometry, import it into the initialized network, register the network's
callback, then start both peers. Both peers must use the same authored input
file. The surface peer must omit the 1D binary output path.

.. code-block:: python

   from openswmm.engine import SplitRole

   network.split.validate_input()
   network.split.configure(SplitRole.NETWORK)
   surface.split.configure(SplitRole.SURFACE)
   surface.initialize()
   with surface.split.geometry() as geometry:
       network.initialize()
       network.split.apply_geometry(geometry)

   def exchange(phase, request):
       return surface.split.evaluate(phase, request)

   network.split.set_callback(exchange)
   surface.start(save_results=False)
   network.start(save_results=True)
   for elapsed in network.steps():
       pass
   network.end()

A callback request is borrowed, restricted to its callback thread, and expires
when the callback returns. The callback must return a newly owned frame from
``evaluate``; returning the borrowed request is an error. The native host
consumes the returned reply, so its Python wrapper becomes invalid immediately.
Owned geometry and decoded frames support ``close`` and the context manager.
Keep the receiving engine alive through the callback. Python exceptions are
captured at the C boundary and re-raised after the native operation; completed
physics is not rolled back. Callback registration and removal require the
``INITIALIZED`` network state, before start.

``SplitFrame.node`` returns SI values. ``source`` and ``outfall_return`` expose
the documented native ft³/s and ft³ receipts only when the native reviewed-source
profile supplies them. ``encode`` and ``decode`` copy versioned UTF-8 JSON and
never advance either engine. They require the optional native packet codec;
otherwise they raise ``PluginError``. Decoded packets expose only the quantities
published for their phase, rather than a complete native node snapshot.

``solver.set_phase_callback(observer)`` registers a read-only diagnostic
observer receiving ``(phase, start_seconds, end_seconds)``. It may read public
state but must not mutate or advance an engine. Pass ``None`` between steps to
unregister it. Observer exceptions are re-raised after the current native step.

Atomic editor records
---------------------

The following operations are available before initialization. Native validation
failures preserve the authored state, including a failure in the last member.

* ``node.divider.params`` reads or atomically replaces the complete divider
  record. Flow and length fields use the project's display units.
* ``solver.surface2d.validate_node_coupling`` and ``replace_node_coupling``
  accept a complete parameter mapping and its authored-row flag.
* ``solver.infrastructure.inlet_usages.validate`` preflights a usage mapping.
* ``solver.infrastructure.lids.usage_record`` copies every placement field,
  including both report-path strings and the drain target. ``validate_usage``
  checks a record, and ``replace_usages`` atomically replaces one subcatchment's
  ordered records while preserving other owners.
* ``lids.node_snapshot``, ``prepare_node``, ``validate_node`` and ``replace_node``
  expose a complete storage-LID assignment and its keyed anchors. Snapshot fields
  ending in ``_ft`` deliberately retain exact internal feet in both unit systems
  for lossless replay. They are not ordinary project-unit editor inputs.

Inlet designs can be checked without modifying the model with
``solver.infrastructure.inlets.validate_design(record)``. The returned record
contains the normalized active fields and the resolved custom curve kind;
it can be passed directly to ``set_design`` after review.
