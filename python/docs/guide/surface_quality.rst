Surface water quality
=====================

``solver.surface2d.quality`` exposes 2D land-use coverage, initial buildup and
curb-length tables. Acquire it after ``solver.open()`` and edit before
initialization. The view retains its solver and becomes stale after structural
or lifecycle changes; reacquire it after adding land uses or pollutants.

``CellScope`` selects GLOBAL, TAG or CELL. More specific rows override broader
ones at runtime. Cell indices are zero-based for both triangles and quads.

.. code-block:: python

   from openswmm.engine import CellScope

   quality = solver.surface2d.quality
   quality.set_coverage(CellScope.GLOBAL, {'URBAN': 100})
   quality.set_loading(CellScope.GLOBAL, 'TSS', 10)
   quality.set_loading(CellScope.TAG, 'TSS', 20, tag='PAN')
   quality.set_curb_length(CellScope.GLOBAL, 25)

``set_coverage`` replaces the entire land-use mapping at a scope. Values are
percentages; names must identify existing land uses and the engine validates
the total. Use ``remove_coverage(index)`` to clear a row. The ``coverages``,
``loadings`` and ``curb_lengths`` properties are immutable snapshots. Removing
a row shifts subsequent indices.

Initial loading and runtime ``buildup(species)`` use project pollutant mass per
acre/hectare. Curb length uses project length units. ``buildup`` requires an
active surface-quality kernel and returns an owned float64 array in cell order.
Changing the returned array never changes the model. The native buildup and
washoff configuration determines whether the kernel is active.

.. automodule:: openswmm.engine._surface_quality
   :members:
