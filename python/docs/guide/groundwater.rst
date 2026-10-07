Two-zone groundwater
====================

``solver.surface2d.groundwater`` exposes aquifer authoring and runtime results.
The view retains its solver and is invalidated when the solver closes or its
structure changes. Edits require BUILDING or OPENED. Use an explicit
``solver.open()`` before editing; the solver context manager starts the run.

Authoring units and precedence
------------------------------

``add_row(scope, ks, zs, theta_s, theta_r, alpha, tag="", cell=-1)`` appends
an aquifer row. ``CellScope`` selects GLOBAL, TAG or CELL. Runtime resolution
uses the more specific row; cells are zero-based in Python on both triangular
and quadrilateral meshes.

* ``ks`` uses project rainfall-rate units (in/hr or mm/hr).
* ``zs`` uses project length (ft or m), and ``alpha`` inverse length.
* ``theta_s`` and ``theta_r`` are dimensionless water contents.
* Optional row properties include HG0, PSI_B, LAMBDA, N, L, C_LOSS,
  SOIL_CHAR, CLOSURE and M_LAYERS. Use ``GroundwaterSoil`` and
  ``GroundwaterClosure`` for their numeric selectors.

``options`` is a text mapping using native INP keys and values. ``rows`` and
``nodes`` are immutable snapshots. Removing a row shifts subsequent indices.
``add_node`` accepts a zero-based cell or -1 for coordinate-based location;
its conductivity is in/hr or mm/hr, thickness is ft or m, and area is ft² or
m². Zero area uses cell area, and zero conductivity requests direct Darcy
exchange. Node snapshots distinguish authored, located and automatically
enrolled beds and expose the exchange opt-out flag.

Runtime units
-------------

After initialization/start, ``active`` indicates that the kernel is available.
Runtime hydrology uses SI regardless of the input project's unit system.
``dimensions`` returns cell and sigma-layer counts. ``cell(index, variable)``
reads one value and ``cells(variable)`` returns an owned float64 NumPy snapshot.
``GroundwaterVariable`` selectors distinguish thickness/elevation (m), flux
rates (m/s or m³/s), timestep (s), tier and closure codes.

``column(cell)`` returns water contents from the surface down and requires a
SIGMA closure; the engine refuses other closures instead of returning a
misleading zero-filled array. ``ledger(term)`` returns m³. The
``continuity_error`` is a residual in m³, not a percentage.
``tier_histogram`` uses the platform's C-long dtype, including on Windows.

``species`` lists the transported names in native policy order.
``concentrations(zone, species_index)`` returns per-cell concentrations in the
species' own mass units per m³ of zone water; dry zones report zero.
``species_ledger`` exposes the corresponding cumulative native ledger.

.. automodule:: openswmm.engine._groundwater
   :members:

Transport authoring
-------------------

``groundwater.transport`` edits the groundwater transport tables before
initialization. Its ``options`` mapping takes native text keys such as
TRANSPORT_POLLUTANTS, TRANSPORT_MSX, TRANSPORT_AGE and TRANSPORT_TEMPERATURE.
Use YES/NO for switches. ``authored`` distinguishes an authored transport
configuration from an absent one; consult ``solver.transport_matrix`` for
runtime availability.

The ``parameters``, ``sorption``, ``initial_quality``, ``boundaries`` and
``sources`` properties return tuples of frozen records. Their ``set_*`` methods
upsert by native row identity. Change a field with ``dataclasses.replace`` and
submit the new record; modifying a snapshot does not mutate the model.
``remove_*`` takes a zero-based row index, and subsequent indices shift.
Source-species terms have a separate table indexed by the source row.

.. code-block:: python

   from openswmm.engine import GroundwaterParameters, GroundwaterInitialQuality

   transport = solver.surface2d.groundwater.transport  # after solver.open()
   transport.options['TRANSPORT_POLLUTANTS'] = 'YES'
   transport.set_parameters(GroundwaterParameters(rho_s=2700))
   transport.set_initial_quality(
       GroundwaterInitialQuality(species='TSS', value=5)
   )

Parameter records use SI: density kg/m³, heat capacity J/(kg K), conductivity
W/(m K), dispersivity m, diffusivity m²/s and geothermal flux W/m². Source flow
is m³/s regardless of project FLOW_UNITS. Sorption ``kd`` is L/kg and decay is
1/day; a negative decay inherits the pollutant value. Initial quality uses
native species authoring units, which must not be confused with runtime
mass-per-m³ arrays.

``GroundwaterTransportZone`` selects SAT, UNSAT or LAYER. LAYER requires the
native one-based layer number; cell, edge and table indices are zero-based.
Boundary edge indices follow the cell geometry, including edge 3 of a quad.
``initial_quality_file`` accepts a path or None to clear the reference. The
reference is authored metadata; it does not read or apply the file immediately.

.. automodule:: openswmm.engine._gw_transport
   :members:
