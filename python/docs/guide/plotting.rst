========
Plotting
========

.. note::

   **Engine:** OpenSWMM 6 — refactored.

.. currentmodule:: openswmm.engine

The bindings hand back numpy arrays with native dtypes, so plotting is
mostly a one-liner. This page collects the recipes that matter most.

----

Time-series from the output file
================================

:attr:`OutputReader.period_times` is a ``datetime64[s]`` numpy array
and :meth:`OutputReader.node_series` returns ``float32`` values — both
plug directly into matplotlib:

.. code-block:: python

    import matplotlib.pyplot as plt
    from openswmm.engine import OutputReader, OutNodeVar

    with OutputReader("model.out") as out:
        depth = out.node_series("J1", OutNodeVar.DEPTH)
        head  = out.node_series("J1", OutNodeVar.HEAD)

        fig, ax = plt.subplots(figsize=(8, 4))
        ax.plot(out.period_times, depth, label="depth")
        ax.plot(out.period_times, head,  label="head")
        ax.set_ylabel("ft")
        ax.legend()
        fig.autofmt_xdate()

For multiple nodes on one plot:

.. code-block:: python

    fig, ax = plt.subplots()
    for nid in ("J1", "J2", "OUT1"):
        ax.plot(out.period_times,
                out.node_series(nid, OutNodeVar.DEPTH),
                label=nid)
    ax.legend()

----

Bulk snapshots during a run
===========================

When the engine is still open, each ``solver.<domain>.<bulk_prop>``
returns a fresh numpy array. Accumulate snapshots inside the step loop
and stack them at the end:

.. code-block:: python

    import numpy as np
    from openswmm.engine import Solver

    history = []
    with Solver("model.inp", "model.rpt") as s:
        for elapsed in s.steps():
            history.append(s.nodes.depths.copy())   # .copy() — see below

    depths = np.stack(history)        # shape (T, n_nodes)

Why ``.copy()``? The array returned by ``solver.nodes.depths`` shares
the internal scratch buffer; the engine overwrites it on the next
call. For one-off reads inside a loop iteration you can skip the copy;
for accumulation across steps you cannot.

----

Spatial plots
=============

:attr:`Solver.spatial` exposes node coordinates and link geometry as
numpy arrays. For a quick scatter:

.. code-block:: python

    import matplotlib.pyplot as plt

    coords = s.spatial.node_coords()       # shape (n_nodes, 2)
    plt.scatter(coords[:, 0], coords[:, 1])

For full network drawing, pair the node coordinates with the
``link.from_node`` / ``link.to_node`` connectivity:

.. code-block:: python

    coords = s.spatial.node_coords()
    fig, ax = plt.subplots()
    for link in s.links:
        a = coords[link.from_node.index]
        b = coords[link.to_node.index]
        ax.plot([a[0], b[0]], [a[1], b[1]], 'k-', lw=0.5)
    ax.scatter(coords[:, 0], coords[:, 1], c="C0", s=8)

----

Aligning the engine-side bulk arrays with ``period_times``
==========================================================

The bulk arrays from :class:`Solver` are sampled on the **routing
step**; :attr:`OutputReader.period_times` is on the **report step**.
They do not align one-to-one. If you need both axes on the same plot,
either:

* Read the same variable from :class:`OutputReader` (report-step
  resolution, exact match with ``period_times``), or
* Track your own ``current_datetime`` in the step loop and feed that
  as the time axis:

.. code-block:: python

    times, depths = [], []
    with Solver("model.inp") as s:
        for _ in s.steps():
            times.append(s.current_datetime)
            depths.append(s.nodes["J1"].depth)

    ax.plot(times, depths)

----

See also
========

* :doc:`output_reader` — every method that yields a plot-ready array.
* :doc:`datetime` — why ``period_times`` is ``datetime64[s]``.
* :doc:`spatial` — coordinates and link vertices.
