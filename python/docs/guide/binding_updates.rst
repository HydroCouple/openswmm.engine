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

Richards LIDs and surface ownership
-----------------------------------

``solver.subcatchments[name].snowpack`` reads the assigned snow-pack name
(empty when unassigned). Assign a name before initialization, or assign
``None`` to clear it. An unknown name leaves the previous assignment intact.

``solver.infrastructure.lids.get_flow_options(control)`` reports the flow
model (0 for the existing formulation or 1 for Richards), numerical cells
per porous layer, absolute/relative tolerances and maximum step in seconds.
``get_materials(control)`` returns retention mappings in authored layer order;
alpha and specific storage use inverse metres in both unit systems.
``set_layers(control, layers, flow=options, materials=materials)`` changes
the stack and flow model atomically. Supply one material mapping per layer;
SURFACE/BOTTOM entries are ignored. Omitted treatments retain their current
values; pass an empty list to remove them. Invalid input leaves the model
unchanged. ``richards_profile(node)`` reports live cell pressure/head in
project length units and complete water in project volume units.
``richards_statistics(node)`` reports the last interval's solver counters,
minimum step in seconds and water-balance residual in cubic metres.

``solver.surface2d.infiltration.authored_rows()`` returns ordered raw records
with ``cell``, ``tag``, ``row`` and ``dest_explicit`` fields.
``replace_authored_rows(records)`` restores a complete set before initialization,
including duplicate/order information and obsolete destinations needed for
undo. Acceptance for authoring does not enable an obsolete runtime destination.
``ownership(cell)`` and ``ownership_bulk()`` expose resolved owner, aquifer row
and conflict codes without modifying these records.

``solver.surface2d.groundwater.options.set_process_options(et=..., link=...,
wilting=..., authored=...)`` atomically changes groundwater ET, conduit
exchange and wilting suction. Custom suction is in project length units;
``authored`` restores section provenance for undo.

``solver.surface2d.get_report_rainfall_bulk(report_date)`` reads report-instant
rainfall in metres per second. The date is an absolute SWMM DateTime in
decimal days; routing-window rainfall remains available through
``get_rainfall_bulk()``.

Surface ownership is currently an authoring/review facility. While editing,
``get_surface_owners()`` reads zero-based subcatchment indices and
``preview_surface_owners(rows)`` reviews a proposed complete set. Passing
``None`` reviews the stored set. The result contains source objects, cell
shares and remaining mesh weather areas in square metres, blocking diagnostics,
``valid`` and a revision ``token``. Use that token with
``replace_surface_owners(rows, token)``; a stale token or invalid proposal
leaves the model unchanged. An empty list clears the records. Initialization
currently rejects stored ownership records until the completed-interval
runtime adapter is qualified; geometric acceptance alone does not activate
recharge or suppress existing weather sources.
