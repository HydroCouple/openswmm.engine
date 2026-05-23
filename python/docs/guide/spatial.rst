=====================================
Spatial  (CRS, coordinates, geometry)
=====================================

.. note::

   **Engine:** OpenSWMM 6 — refactored.  Documents
   :class:`openswmm.engine.Spatial`.  This is unique to the v6 engine.

.. currentmodule:: openswmm.engine

The :class:`Spatial` class manages the geometric / cartographic
metadata of the model:

* **CRS** — Coordinate Reference System (EPSG code or WKT string).
* **Coordinates** — point coordinates for nodes, links, gages, and
  subcatchment centroids.
* **Geometry** — link vertices (polylines) and subcatchment polygons.

Reference: ``openswmm_spatial.h``.

----

Class signature
===============

.. code-block:: python

    class Spatial:
        def __init__(self, solver: Solver) -> None: ...

----

Key methods
===========

CRS
---

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - Action / returns
   * - :meth:`get_crs()`
     - The model's CRS string (EPSG code, PROJ string, or WKT).
   * - :meth:`set_crs(crs)`
     - Set the CRS.  Accepts ``"EPSG:4326"``, full WKT, etc.

Coordinates
-----------

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Method
     - Action / returns
   * - :meth:`get_node_coord(idx)` / :meth:`set_node_coord(idx, x, y)`
     - Node point coordinates.
   * - :meth:`get_node_coords_bulk()` / :meth:`set_node_coords_bulk(...)`
     - Bulk read / write of every node's ``(x, y)``.
   * - :meth:`get_link_coord(idx)` / :meth:`set_link_coord(idx, x, y)`
     - Link "centroid" / annotation point.
   * - :meth:`get_subcatch_coord(idx)` / :meth:`set_subcatch_coord(idx, x, y)`
     - Subcatchment centroid.
   * - :meth:`get_gage_coord(idx)` / :meth:`set_gage_coord(idx, x, y)`
     - Rain gage location.

Link vertices  (polylines)
--------------------------

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Method
     - Action / returns
   * - :meth:`get_link_vertex_count(idx)`
     - Number of intermediate vertices on link ``idx``.
   * - :meth:`get_link_vertices(idx)`
     - Return all vertex ``(x, y)`` coordinates.
   * - :meth:`set_link_vertices(idx, ...)`
     - Replace the vertex list.

Subcatchment polygons
---------------------

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Method
     - Action / returns
   * - :meth:`get_subcatch_polygon_count(idx)`
     - Number of vertices on the subcatchment polygon.
   * - :meth:`get_subcatch_polygon(idx)`
     - All polygon ``(x, y)`` coordinates.
   * - :meth:`set_subcatch_polygon(idx, ...)`
     - Replace the polygon.

----

End-to-end example
==================

.. code-block:: python

    from openswmm.engine import Solver, Spatial, Nodes

    with Solver("urban.inp", "urban.rpt", "urban.out") as s:
        spatial = Spatial(s)
        nodes = Nodes(s)

        print("CRS:", spatial.get_crs() or "<none>")

        # Print every junction's coordinates
        for i in range(nodes.count()):
            x, y = spatial.get_node_coord(i)
            print(f"  {nodes.get_id(i):<12}  ({x:.2f}, {y:.2f})")

----

Common recipes
==============

Set a CRS at edit time
----------------------

.. code-block:: python

    s.open()
    spatial.set_crs("EPSG:6433")           # Idaho West (US Survey Feet)
    s.initialize()

Bulk-read every node coordinate into NumPy
-------------------------------------------

.. code-block:: python

    coords = spatial.get_node_coords_bulk()    # ndarray, shape (n_nodes, 2)
    # → coords[:, 0] = x, coords[:, 1] = y

Re-project node coordinates with pyproj
---------------------------------------

.. code-block:: python

    import numpy as np
    from pyproj import Transformer

    src_crs = spatial.get_crs() or "EPSG:6433"
    dst_crs = "EPSG:4326"
    tx = Transformer.from_crs(src_crs, dst_crs, always_xy=True)

    coords = spatial.get_node_coords_bulk().copy()      # detach from scratch
    lon, lat = tx.transform(coords[:, 0], coords[:, 1])
    new = np.column_stack([lon, lat])

    spatial.set_node_coords_bulk(new)
    spatial.set_crs(dst_crs)

Walk every link vertex
----------------------

.. code-block:: python

    for i in range(links.count()):
        n = spatial.get_link_vertex_count(i)
        if n > 0:
            verts = spatial.get_link_vertices(i)
            print(f"{links.get_id(i):<12} has {n} vertices")
            for x, y in verts:
                print(f"     ({x:.2f}, {y:.2f})")

Export every subcatchment polygon to GeoJSON
--------------------------------------------

.. code-block:: python

    import json

    features = []
    for i in range(sc.count()):
        n = spatial.get_subcatch_polygon_count(i)
        if n < 3:
            continue
        ring = spatial.get_subcatch_polygon(i)
        ring_closed = list(ring) + [ring[0]]
        features.append({
            "type": "Feature",
            "id":   sc.get_id(i),
            "geometry": {"type": "Polygon", "coordinates": [ring_closed]},
            "properties": {"id": sc.get_id(i)},
        })

    with open("subcatchments.geojson", "w") as f:
        json.dump({"type": "FeatureCollection", "features": features}, f)

----

Bulk arrays
===========

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Shape
   * - :meth:`get_node_coords_bulk`
     - ``np.ndarray[float64]``, shape ``(n_nodes, 2)``.
   * - :meth:`set_node_coords_bulk(arr)`
     - Same shape; updates every node coordinate at once.

For per-link or per-subcatchment polygons there is no bulk surface —
the vertex counts vary per object, so you must walk them.

----

EngineState requirements & exceptions
=====================================

.. list-table::
   :header-rows: 1
   :widths: 30 25 45

   * - Method group
     - Required state
     - Notes
   * - read accessors
     - ``OPENED`` or later
     - n/a
   * - setters (CRS, coords, vertices)
     - ``OPENED``
     - Geometry is metadata; mid-run changes do not affect routing.

Common :class:`EngineError` codes:

* ``INVALID_INDEX`` — out-of-range index.
* ``INVALID_TYPE``  — too few vertices for a polygon (≥ 3 required).

----

See also
========

* :doc:`hotstart` — CRS captured in / restored from hot-start files.
* :doc:`nodes`, :doc:`links`, :doc:`subcatchments` — the objects
  whose geometry this class manages.
