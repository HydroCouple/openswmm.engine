======================
Working with results
======================

.. currentmodule:: openswmm.engine

``Results`` provides named, lazy fields over saved 1D and 2D output. It owns
independent readers and does not need an active solver. Existing
``OutputReader`` methods remain available; see :doc:`output_reader`.

Open files and inspect fields
=============================

.. code-block:: python

   from openswmm.engine import Results

   with Results.open("model.out", surface="model.2d.h5") as results:
       depth = results.nodes["J1"].depth
       print(depth.shape, depth.dimensions, depth.units)
       values = depth.read()                 # NumPy array
       times = depth.times                  # NumPy datetime64 array

       flows = results.links["C1"].flow.read()
       flood_map = results.surface.depth.read(periods=-1)
       cell_history = results.surface.cells[42].depth.read()

The 1D path and ``surface`` path are independently optional; provide at least
one. A surface-only result has no 1D collections. ``results.surface`` is
``None`` when no surface file was supplied. Paths accept ``pathlib.Path``.
Missing files raise ``FileError``. Always close readers, usually through a
context manager. Loaded arrays remain valid after close; further reads fail.

Fields and selection
====================

Attribute completion and ``variables`` show recorded fields:

.. code-block:: python

   with Results.open("model.out", surface="model.2d.h5") as results:
       print(results.nodes.variables.keys())
       print(results.surface.variables.keys())

       # All objects: [time, object]. One object: [time].
       all_depths = results.nodes.depth.read()
       chosen = results.nodes.depth.read(
           objects=["J1", "J2"], periods=slice(10, 20))
       last_depth = results.nodes["J1"].depth[-1]
       every_other = results.nodes["J1"].depth[::2]

       # A subset of native mesh cells: [time, cell].
       cells = results.surface.depth.read(
           objects=[4, 9, 12], periods=slice(0, 10))
       one_cell = results.surface.depth[:, 4]

Integers select one item and remove that axis. Slices use Python's exclusive
stop and support negative indices and steps. Ordered object lists retain order
and duplicates; empty selections return correctly shaped empty arrays. Lists
are supported by ``objects``; the native reader accepts integer/slice axes.
Use either ``selection=`` (also available through square brackets) or named
selectors in one call.

Fields do not load numeric values until ``read()``. Selected 2D periods/cells
use native HDF5 hyperslabs. Reading the whole field explicitly loads it into
memory. One-dimensional histories use the existing native series reader.
``Results`` adds no pandas, xarray, or h5py requirement.

Times and units
===============

``results.times`` contains the 1D reporting dates. ``results.surface.period_times``
contains the 2D dates; ``REPORT_2D_STEP`` can produce a different cadence.
The interface never interpolates or joins those clocks implicitly.

The 2D reader decodes the file's declared CF time units. Current files use
days since 1899-12-30 00:00:00, returned as ``datetime64[us]``. Dates carry no
timezone. Static envelopes, mesh geometry, and independent diagnostic records
have no hydraulic report clock. Use each field's ``dimensions`` and ``times``.
A report field whose row count differs from the time coordinate raises an
error when requesting ``times``; raw selections remain available for inspection.

1D fields retain the file's project units. Rainfall/infiltration are in in/h
or mm/h; evaporation/PET are in in/day or mm/day. 2D fields retain their stored
units, normally SI, including for US-unit models. ``units`` is ``None`` where
the file does not identify a unit. Concentration fields use stored species
unit codes; the binary format cannot recover arbitrary original MSX unit
labels that its writer mapped to a legacy concentration code.

Species fields
==============

.. code-block:: python

   with Results.open("model.out", surface="model.2d.h5") as results:
       tss_1d = results.nodes["J1"].species["TSS"].read()
       tss_2d = results.surface.species["TSS"].read()
       cell_tss = results.surface.cells[42].species["TSS"].read()
       selected = results.surface.species_conc.read(
           species="TSS", objects=[0, 1], periods=-1)

Species names follow the file, including ``REPORT_2D_SPECIES`` filtering.
The 1D reserved columns ``__WATER_AGE__`` and ``__TEMPERATURE__`` use hours and
degrees C. 2D species units come from the file's per-species metadata; they
must not be inferred from the 1D convention. Unrecorded species raise
``KeyError``; they are not returned as zero concentrations.

Mesh and coordinate reference
=============================

.. code-block:: python

   with Results.open(surface="model.2d.h5") as results:
       mesh = results.surface.mesh
       print(mesh.vertices.shape)           # [vertex, xyz]
       print(mesh.cells.shape)              # [cell, corner]
       print(mesh.vertex_counts)            # 3 for triangles, 4 for quads
       print(mesh.crs, mesh.metres_per_model_unit)

Cells retain native simulation order. Connectivity is normalized to zero-based
indices and unused corners are -1. Triangle-only files can have three columns;
mixed meshes have four. No display triangulation changes cell identities.

Stored coordinates are normally metres. ``crs`` describes the authored model's
CRS; stored coordinate = model coordinate × ``metres_per_model_unit``. A model
CRS expressed in feet must not be assigned directly to the stored metric
coordinates. Older files without the explicit relationship return ``None``
for missing metadata; the reader does not guess a CRS or conversion factor.

Groundwater, heat, snow and diagnostic groups
=============================================

All recorded numeric datasets, scalar balances and string identity tables
are accessible through ``surface.reader.datasets`` and ``surface.field(path)``.
This includes fields beyond the common surface depth/velocity variables.

.. code-block:: python

   with Results.open(surface="model.2d.h5") as results:
       surface = results.surface
       print(surface.mass_balance)
       print(surface.reader.datasets)

       # When layered groundwater transport was recorded:
       field = surface.gw_layer_conc
       print(field.dimensions)              # time, species, layer, cell
       values = field.read(species="TSS", objects=[0, 1], periods=-1)

``surface.field("/group/dataset")`` preserves independent diagnostic record
axes and metadata. Raw ``reader.read(path, selection)`` selects every stored
axis explicitly. Unknown axes retain positional names instead of receiving
an invented time or cell meaning. Scalar/group attributes are available through
``reader.attributes(path)``. Attribute values are text, with comma-separated
numeric vectors. Small string identity tables can be read with ``strings(path)``.
Missing optional output fields raise ``KeyError`` through ``field()`` and
``AttributeError`` through attribute access. Availability depends on the
simulation and output-variable selections.

Opening from a solver
=====================

.. code-block:: python

   from openswmm.engine import Solver

   with Solver("model.inp", "model.rpt", "model.out") as solver:
       for _ in solver.steps():
           pass
       solver.end()
       with solver.open_results() as results:
           # Includes a configured 2D output file while the model is open.
           depths = results.nodes["J1"].depth.read()

The returned readers belong to ``Results`` and close separately from the
solver. After the solver has been destroyed, provide ``surface=`` explicitly
to include its 2D file. The configured binary output path is retained.

Live access and feature availability
====================================

``Results.open("model.out", live=True)`` and ``results.refresh()`` support the
existing 1D live-file contract. ``solver.open_results(live=True)`` opens its
configured binary output during a run. See :doc:`binding_updates` for complete
and partial period behavior.

Read 2D HDF5 files after the writer closes, or read an immutable published
copy. The native 2D API does not offer live concurrent writer access. Writer
flushing alone is insufficient to establish that contract.

``Output2DReader.available()`` checks whether the linked engine was built with
2D HDF5 result support. The class remains importable in minimal builds;
opening a file there raises ``PluginError``. Core 1D results remain available.

Optional conversions
====================

With pandas or xarray installed, fields support ``to_pandas()`` and
``to_xarray()``. These methods load the complete selected field. Pandas supports
one/two-dimensional fields; xarray retains named dimensions for layered fields.
Both retain unit metadata. Bind an object or species first when appropriate:

.. code-block:: python

   with Results.open("model.out", surface="model.2d.h5") as results:
       series = results.nodes["J1"].depth.to_pandas()
       cells = results.surface.species["TSS"].to_xarray()

Migration
=========

.. list-table::
   :header-rows: 1

   * - Existing method
     - Named equivalent
   * - ``out.node_series("J1", OutNodeVar.DEPTH)``
     - ``results.nodes["J1"].depth.read()``
   * - ``out.link_result(5, OutLinkVar.FLOW)``
     - ``results.links.flow.read(periods=5)``
   * - ``out.node_series("J1", OutNodeVar.DEPTH, start=10, end=20)``
     - ``results.nodes["J1"].depth.read(periods=slice(10, 21))``

Existing methods keep their inclusive ``end`` argument and existing return
types. New field slices follow Python's exclusive stop convention.
