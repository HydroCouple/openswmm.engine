==================================================
Infrastructure  (transects, streets, inlets, LIDs)
==================================================

.. note::

   **Engine:** OpenSWMM 6 — refactored.

.. currentmodule:: openswmm.engine

``solver.infrastructure`` groups four hydraulic-infrastructure families
under one top-level namespace:

* :attr:`Infrastructure.transects` — irregular cross-sections.
* :attr:`Infrastructure.streets` — street cross-section templates.
* :attr:`Infrastructure.inlets` — inlet usage on conduits.
* :attr:`Infrastructure.lids` — LID control definitions + per-subcatchment usage.

Reference: ``openswmm_infrastructure.h``.

----

Quickstart
==========

.. code-block:: python

    from openswmm.engine import Solver, LidType

    with Solver("model.inp") as s:
        infra = s.infrastructure

        # Transects.
        idx = infra.transects.add("T1")
        infra.transects.set_roughness(idx, n_left=0.05, n_right=0.05, n_channel=0.03)
        infra.transects.add_station(idx, station=0.0, elevation=100.0)

        # Streets.
        sidx = infra.streets.add("ST1")
        infra.streets.set_params(
            sidx,
            t_crown=10.0, h_curb=0.5, sx=0.02, n_road=0.016,
            sides=2,
        )

        # Inlets.
        iidx = infra.inlets.add("IN1", "GRATE")
        infra.inlets.set_params(
            iidx, length=2.0, width=1.0, grate_type="P_BAR-50",
        )

        # LIDs.
        lidx = infra.lids.add("BC1", LidType.BIO_CELL)
        infra.lids.set_surface(lidx, storage=0.0, roughness=0.0, slope=0.5)
        infra.lids.set_soil(
            lidx, thick=12.0, porosity=0.45, fc=0.2, wp=0.1,
            ksat=0.5, kslope=10.0)
        infra.lids.set_storage(lidx, thick=12.0, void_frac=0.75, ksat=0.5)
        infra.lids.set_drain(lidx, coeff=0.0, expon=0.5, offset=6.0)

        # Place a LID control on a subcatchment.
        infra.lids.usage_add(
            "S1", lidx, number=1, area=100.0, width=10.0,
            init_sat=0.0, from_imperv=25.0)

----

Surface
=======

Each sub-view exposes ``__len__`` plus ``add`` and the relevant
parameter setters. The C API has no generic id→index lookup for these
families, so the views are intentionally flat — no ``__getitem__``
collection protocol.

Transects
---------

``transects.add(id) → int``
   Returns the integer index of the new transect.

``transects.set_roughness(idx, n_left, n_right, n_channel)``
   Set Manning's roughness for the three transect zones.

``transects.add_station(idx, station, elevation)``
   Append a profile station to a transect.

Streets
-------

``streets.add(id) → int`` — returns the integer index.

``streets.set_params(idx, *, t_crown, h_curb, sx, n_road, ...)``
   Configure the street cross-section. All parameters are keyword-only
   for clarity.

Inlets
------

``inlets.add(id, inlet_type) → int``

``inlets.set_params(idx, *, length, width, grate_type, open_area, splash_veloc)``

LIDs
----

``lids.add(id, lid_type) → int`` — ``lid_type`` is a
:class:`LidType` enum.

``lids.set_surface(idx, *, storage, roughness, slope)``
``lids.set_soil(idx, *, thick, porosity, fc, wp, ksat, kslope)``
``lids.set_storage(idx, *, thick, void_frac, ksat)``
``lids.set_drain(idx, *, coeff, expon, offset)``

``lids.usage_add(subcatchment, lid, *, number, area, width, init_sat, from_imperv)``
   Place a LID control on a subcatchment. ``subcatchment`` accepts
   ``int | str``; ``lid`` is currently an integer index because the C
   API has no id→index lookup for LID controls (passing a string
   raises :exc:`TypeError`).

The placement rows are also readable and removable by global index, so the
``[LID_USAGE]`` block round-trips:

.. code-block:: python

    n = infra.lids.usage_count()              # total placement rows
    row = infra.lids.usage_get(0)             # dict of one row
    print(row["subcatch_index"], row["lid_index"],
          row["area"], row["from_imperv"])
    infra.lids.usage_remove(0)                # delete a placement row

``usage_get`` returns a dict with keys ``subcatch_index``, ``lid_index``,
``number``, ``area``, ``width``, ``init_sat``, ``from_imperv``, ``to_perv``
and ``from_perv``.

----

See also
========

* :doc:`subcatchments` — LID-bearing subcatchments and their
  ``coverage`` mapping.
* :doc:`links` — irregular cross-sections via :attr:`Link.xsect`
  (``XSectShape.IRREGULAR`` references a transect id).
* :doc:`error_handling`.


Storage-node LIDs with ordered layers
-------------------------------------

Use ``LidType.NODE`` for an arbitrary ordered stack. A node remains a storage
node; assigning the control synchronizes its maximum depth. All edits below
are made before initialization, on an opened model containing storage ``S``
and a link ``Underdrain`` connected to it::

    from openswmm.engine import LidType, LidNodeLayer, LidNodeLayerKind as K

    lids = solver.infrastructure.lids
    lids.add("Column", LidType.NODE)
    lids.set_layers("Column", [
        LidNodeLayer(K.SURFACE, (6, 0.1)),
        LidNodeLayer(K.MEDIA, (12, .45, .20, .08, 2, 10, 3)),
        LidNodeLayer(K.MEDIA, (6, .40, .25, .10, 1, 8, 3)),
        LidNodeLayer(K.AGGREGATE, (6, .40, 100)),
        LidNodeLayer(K.BOTTOM, (.5, 0)),
    ])
    lids.assign_node("S", "Column", initial_saturation=10)
    lids.set_outlet_anchor("Underdrain", 4, position="BOTTOM")
    assert lids.node_assignment("S") == (lids.get_index("Column"), 10.0)

Thickness and suction use inches or millimetres; conductivity and seepage
use inches/hour or millimetres/hour. MEDIA and AGGREGATE rows can repeat
without a fixed limit. SURFACE must be first and BOTTOM last when present.
BOTTOM is a boundary, excluded from the one-based outlet layer numbering.
Invalid stack replacements are atomic and leave the prior configuration
unchanged, including node depths and outlet offsets.

``get_layers(control)`` returns the authored rows, or normalized layers for
a supported standard control. ``node_profile(node)`` returns runtime cell
dictionaries with ``layer``, ``bottom``, ``top``, and ``moisture``. Bottom/top
are heights above the storage invert in feet or metres; moisture is a
volumetric fraction. The profile is empty before initialization. Node volume
includes retained moisture plus mobile water.

``remove_node(node)`` reverts to ordinary storage and removes connected
anchors. ``set_outlet_anchor(link, 0)`` removes only that anchor. A link with
two LID endpoints uses explicit offsets because the anchor syntax identifies
only one endpoint. LID storage routing currently requires Dynamic Wave.
Native hotstarts preserve the moisture profile and clogging history; use
contributing subcatchments or external inflow for rainfall on the LID area.

Layer pollutant treatment
~~~~~~~~~~~~~~~~~~~~~~~~~

A NODE control can assign removal percentages, first-order decay rates, and
optional ``R =`` (removal fraction) or ``C =`` (effluent concentration)
expressions to each physical layer. For an existing pollutant ``TSS``::

    from openswmm.engine import LidLayerTreatment

    layers = lids.get_layers("Column")
    lids.set_layers("Column", layers, treatments=[
        LidLayerTreatment(2, "TSS", removal_percent=25,
                          decay_per_day=1.5, expression="R = 0.2"),
        LidLayerTreatment(3, "TSS", decay_per_day=0.5),
    ])
    rules = lids.get_treatments("Column")

Layer numbers are one-based, top to bottom, including SURFACE. BOTTOM is a
boundary and cannot receive pollutant treatment. Removal must be 0–100%; decay
must be nonnegative and is expressed in 1/day. Blank expressions allow rates
alone. Fixed removal is applied first, followed by the expression; for example,
25% removal followed by ``R = 0.2`` gives 40% combined removal. Expressions use
the existing treatment grammar and cannot create pollutant mass.

Removal and expressions act at authored-layer exits, not at internal numerical
subcells. Layer decay supplements pollutant background decay. First-order decay acts on
resident retained mass; submerged layers
contribute volume-weighted decay to the shared saturated storage reactor.
An outlet drawing saturated water uses the removal rule of its physical layer.
A surface bypass therefore does not pass through every media treatment rule.
Concentrations use the pollutant's project units, ``DT`` is seconds, ``HRT`` is
hours, and flow, depth and area use project units. ``V`` retains the existing
expression engine's internal cubic-foot convention.

``treatments=None`` (the default) preserves rules by layer position and kind.
Pass the complete treatment list when reordering layers; ``treatments=[]``
clears all rules. Layer geometry and treatment changes are validated together
and rejected atomically. Configure them before initialization.

INP files persist rules in ``[LID_LAYER_TREATMENT]`` as
``Control Layer Pollutant RemovalPercent DecayPerDay [Expression]``. GeoPackage
files preserve the same definitions; native V9 hotstarts also preserve retained
pollutant masses. Layer pollutant routing currently requires
``QUALITY_SOLVER LEGACY``. As with transient mixed-reactor routing, check quality
continuity and routing-step convergence, particularly during rapid filling.
