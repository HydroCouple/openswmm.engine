==========
Pollutants
==========

.. note::

   **Engine:** OpenSWMM 6 — refactored.

.. currentmodule:: openswmm.engine

Pollutants and their fate parameters. Shape mirrors :doc:`nodes` —
``solver.pollutants`` is the collection, :class:`Pollutant` is the
wrapper.

Reference: ``openswmm_pollutants.h``.

----

Quickstart
==========

.. code-block:: python

    from openswmm.engine import Solver, ConcentrationUnits

    with Solver("model.inp") as s:
        tss = s.pollutants["TSS"]
        print(tss.units, tss.kdecay, tss.init_conc)

        # Decay / fate
        tss.kdecay = 0.05

        # Runtime injection at a node or link.
        s.pollutants.set_node_quality("J1", "TSS", 12.0)
        s.pollutants.set_link_quality("C1", "TSS", 8.0)

----

Collection
==========

.. list-table::
   :header-rows: 1
   :widths: 36 64

   * - Operation
     - What it does
   * - ``len(s.pollutants)`` / ``for p in s.pollutants:`` / ``s.pollutants[key]``
     - Standard container protocol over :class:`Pollutant` wrappers.
   * - ``s.pollutants.get_index(id)`` / ``get_id(idx)``
     - Id ↔ index lookup.
   * - ``s.pollutants.add(id, units=ConcentrationUnits.MG_PER_L)``
     - Append a pollutant. Returns the new :class:`Pollutant`.
   * - ``s.pollutants.set_node_quality(node, pollutant, conc)``
     - Runtime quality injection at a node.
   * - ``s.pollutants.set_link_quality(link, pollutant, conc)``
     - Runtime quality injection in a link.

----

Wrapper: :class:`Pollutant`
===========================

.. list-table::
   :header-rows: 1
   :widths: 22 22 14 42

   * - Property
     - Type
     - Mode
     - Meaning
   * - ``id`` / ``index``
     - ``str`` / ``int``
     - read-only
     -
   * - ``units``
     - :class:`ConcentrationUnits`
     - read-only
     -
   * - ``kdecay``
     - ``float``
     - read/write
     - First-order decay coefficient.
   * - ``mwt``
     - ``float``
     - read/write
     - Molecular weight.
   * - ``rain_conc`` / ``gw_conc`` / ``init_conc`` / ``rdii_conc``
     - ``float``
     - read/write
     - Inflow concentrations.
   * - ``snow_only``
     - ``bool``
     - read/write
     -
   * - ``co_pollutant``
     - ``(Pollutant, float) | None``
     - read-only
     -

Use :meth:`Pollutant.set_co_pollutant(other, fraction)` to set the
co-pollutant link.

----

See also
========

* :doc:`nodes` — :meth:`Node.quality` and ``set_quality_mass_flux``.
* :doc:`links` — :meth:`Link.quality`.
* :doc:`subcatchments` — :meth:`Subcatchment.quality`.
* :doc:`error_handling`.
