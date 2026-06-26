=====================================
Spatial  (CRS, coordinates, geometry)
=====================================

.. note::

   **Engine:** OpenSWMM 6 — refactored.

.. currentmodule:: openswmm.engine

``solver.spatial`` exposes the geographic side of the model: the
coordinate reference system, node/link/subcatchment/gage coordinates,
link polyline vertices, and subcatchment polygons.

Reference: ``openswmm_spatial.h``.

----

Quickstart
==========

.. code-block:: python

    from openswmm.engine import Solver

    with Solver("model.inp") as s:
        # CRS — read or write.
        print(s.spatial.crs)
        s.spatial.crs = "EPSG:4326"

        # Per-object coords (int or str).
        x, y = s.spatial.node_coord("J1")
        s.spatial.set_node_coord("J1", x + 5.0, y)

        # Bulk arrays.
        coords = s.spatial.node_coords()        # shape (n_nodes, 2)
        s.spatial.set_node_coords(coords * 1.0)  # round-trip

        # Geometry — polyline + polygon (numpy arrays).
        verts = s.spatial.link_vertices("C1")    # (n, 2)
        poly = s.spatial.subcatchment_polygon("S1")

----

CRS
===

.. list-table::
   :widths: 30 70
   :header-rows: 1

   * - Property / method
     - What it does
   * - ``s.spatial.crs``
     - ``str`` property. Reads/writes the CRS string (EPSG token, WKT,
       or proj string). Reading a model with no CRS raises
       :class:`CRSError`.

----

Per-object coordinates
======================

All four families have a uniform shape: ``int | str`` object selectors,
``(x, y)`` tuples, plus explicit setters.

.. list-table::
   :widths: 36 30 34
   :header-rows: 1

   * - Method
     - Object selector
     - Returns / accepts
   * - ``node_coord(node)`` / ``set_node_coord(node, x, y)``
     - ``int | str``
     - ``(x, y)`` tuple
   * - ``link_coord(link)`` / ``set_link_coord(link, x, y)``
     - ``int | str``
     - ``(x, y)`` tuple
   * - ``subcatchment_coord(sub)`` / ``set_subcatchment_coord(sub, x, y)``
     - ``int | str``
     - ``(x, y)`` tuple
   * - ``gage_coord(gage)`` / ``set_gage_coord(gage, x, y)``
     - ``int | str``
     - ``(x, y)`` tuple

----

Bulk coordinate arrays
======================

For vectorised work, use the bulk node-coord pair:

.. code-block:: python

    coords = s.spatial.node_coords()       # np.ndarray, shape (n_nodes, 2)
    coords += [0.0, 100.0]                  # shift north by 100 ft
    s.spatial.set_node_coords(coords)

The C API doesn't currently expose bulk equivalents for links,
subcatchments, or gages — those go one at a time.

----

Polyline + polygon geometry
===========================

.. code-block:: python

    verts = s.spatial.link_vertices("C1")       # (n, 2) float64
    poly  = s.spatial.subcatchment_polygon("S1") # (n, 2) float64

    # Replace.
    s.spatial.set_link_vertices("C1", new_verts)
    s.spatial.set_subcatchment_polygon("S1", new_poly)

Both arrays are column-stacked ``(x, y)`` pairs; reshape with NumPy as
needed.

----

See also
========

* :doc:`nodes`, :doc:`links`, :doc:`subcatchments`, :doc:`gages` — the
  topology / property side of the same objects.
* :doc:`plotting` — recipes for drawing the network.
* :doc:`error_handling`.
