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
     - Node point coordinates (returns ``(x, y)`` tuple).
   * - :meth:`get_node_coords_bulk()` / :meth:`set_node_coords_bulk(x, y)`
     - Bulk read / write of every node's coordinates.  Returns a tuple
       of two ``ndarray[float64]`` of shape ``(n_nodes,)`` — *not* a 2-D
       array — so unpack as ``x, y = spatial.get_node_coords_bulk()``.
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
     - Number of polyline vertices on link ``idx`` (includes upstream
       + downstream node coordinates as first and last entries).
   * - :meth:`get_link_vertices(idx)`
     - Tuple ``(x, y)`` of two ``ndarray[float64]`` of shape
       ``(get_link_vertex_count(idx),)``.  Order is upstream node,
       interior vertices, downstream node.
   * - :meth:`set_link_vertices(idx, x, y)`
     - Replace the vertex list; ``x`` and ``y`` are equal-length
       ``ndarray[float64]``.

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
     - Tuple ``(x, y)`` of two ``ndarray[float64]`` of shape
       ``(get_subcatch_polygon_count(idx),)``.
   * - :meth:`set_subcatch_polygon(idx, x, y)`
     - Replace the polygon; ``x`` and ``y`` are equal-length
       ``ndarray[float64]``.

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

    x, y = spatial.get_node_coords_bulk()    # two ndarrays, shape (n_nodes,)

Re-project node coordinates with pyproj
---------------------------------------

.. code-block:: python

    from pyproj import Transformer

    src_crs = spatial.get_crs() or "EPSG:6433"
    dst_crs = "EPSG:4326"
    tx = Transformer.from_crs(src_crs, dst_crs, always_xy=True)

    x, y = spatial.get_node_coords_bulk()      # detach implicitly via assignment
    lon, lat = tx.transform(x, y)

    spatial.set_node_coords_bulk(lon, lat)
    spatial.set_crs(dst_crs)

Walk every link vertex
----------------------

.. code-block:: python

    for i in range(links.count()):
        n = spatial.get_link_vertex_count(i)
        if n == 0:
            continue
        xs, ys = spatial.get_link_vertices(i)
        print(f"{links.get_id(i):<12} has {n} vertices (incl. endpoints)")
        for x, y in zip(xs, ys):
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
        xs, ys = spatial.get_subcatch_polygon(i)
        ring = list(zip(xs.tolist(), ys.tolist()))
        ring_closed = ring + [ring[0]]
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
   * - :meth:`get_node_coords_bulk()`
     - Tuple ``(x, y)`` of two ``ndarray[float64]``, each of shape
       ``(n_nodes,)``.
   * - :meth:`set_node_coords_bulk(x, y)`
     - Same shapes; updates every node coordinate at once.
   * - :meth:`get_link_vertices(idx)` / :meth:`get_subcatch_polygon(idx)`
     - Tuple ``(x, y)`` of equal-length ``ndarray[float64]``.

For per-link or per-subcatchment polylines there is no project-wide
bulk surface — the vertex counts vary per object, so you must walk
them.

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
