Editing, forcing and live output
================================

Batch editing
-------------

``ModelEditor`` retains its engine owner and checks it before every operation.
``delete_nodes``, ``delete_links``, ``delete_subcatchments`` and ``delete_gages``
accept iterables of names or zero-based indices. All refer to the model before
the batch. Duplicates are ignored and invalid indices fail before any deletion.
A successful nonempty batch invalidates element views and returns the aggregate
cascade report. Reacquire views after an edit; the editor itself remains usable.

Live output
-----------

``OutputReader(path, live=True)`` opens an output whose header is complete,
even before the writer finishes. ``refresh()`` returns the number of complete
periods and clears cached timestamps. Partial trailing records are excluded.
``is_live`` becomes false when closing records arrive. This supports monitoring
runs and recovering completed records from interrupted runs; it never modifies
the file. Concurrent access to a reader during a native series read raises
``LifecycleError``; independent readers can operate concurrently.
Close the reader or use its context manager. Reads after close raise
``BadHandleError``.

.. code-block:: python

   from openswmm.engine import OutputReader, OutNodeVar

   with OutputReader('run.out', live=True) as output:
       count = output.refresh()
       if count:
           depths = output.node_result(count - 1, OutNodeVar.DEPTH)

Staged serialization
--------------------

``solver.write_staged(final_path, mapper)`` routes each physical file through
``mapper(final_path, kind)``. Kind 0 identifies the INP file, 1 the mesh and 2 a
component configuration. Return a distinct staging path, or None to refuse.
References inside files are computed from final paths. Success confirms only
serialization; the caller owns validation, cleanup and publication of every
output. Plugin writers are excluded. Engine access inside the mapper raises
``LifecycleError``. Python callback exceptions are re-raised after the native
writer returns safely.

Additional forcing and diagnostics
----------------------------------

``forcing.node_temperature`` uses degrees C for REPLACE, and degrees C times
ft³/s for ADD. ``forcing.node_age`` uses hours for REPLACE and hours times ft³/s
for ADD. Enable heat or water-age transport before initialization respectively.
``forcing.link_seepage`` uses ft³/s even for SI projects, positive out of the
conduit. Negative values supply water from groundwater. If the built-in aquifer
is coupled, the forced exchange also updates that aquifer; disable the internal
reach exchange when an external groundwater model supplies the water.

``forcing.element_climate`` takes ``HeatElemKind``, a node/link name or index,
and an ELEM_* ``ForcingType``. Air temperature and wind use project units;
humidity uses percent and shortwave uses W/m². ``element_climate_get`` returns
``(value, mode)``, with None for mode when no forcing is present. Setters accept
REPLACE or ADD, with the existing one-shot/persistent policy.

``surface2d.get_rainfall_bulk()`` returns m/s. ``get_rain_volume_bulk()`` and
``get_coupling_volume_bulk()`` return cumulative m³, with positive coupling from
1D to 2D. Arrays are independent snapshots covering triangles and quads.
``rainfall_weights(cell)`` returns method, gage indices and weights. Method -1
means not applicable, 0 natural neighbour, 1 inverse distance, and 2 nearest.

``Surface2D.output_variables()`` discovers available output names.
``output_variable_mask(text)`` accepts DEFAULT, MINIMAL, ALL or a variable list;
invalid selections raise ValueError. ``output_variable_text(mask)`` returns the
canonical text for storing the selection.
