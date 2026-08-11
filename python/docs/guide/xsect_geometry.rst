======================
Cross-Section Geometry
======================

.. note::

   **Engine:** OpenSWMM 6 — refactored. This page documents
   :class:`openswmm.engine.XSectionGeometry`, which exposes the engine's
   cross-section geometry kernels directly. Reference: ``openswmm_xsect.h``.

.. currentmodule:: openswmm.engine

Every cross-section question the routing solvers ask — *what is the flow area
at this depth? the top width? the hydraulic radius? the critical depth for this
flow?* — is answered by a small set of geometry kernels.
:class:`XSectionGeometry` exposes those same kernels, so you can use them as a
reference implementation without running a simulation, and get answers that
agree with the engine exactly.

.. code-block:: python

    from openswmm.engine import XSectionGeometry, XSectShape

    pipe = XSectionGeometry(XSectShape.CIRCULAR, 1.0, units="SI")
    pipe.area(0.5)          # 0.3927 m² — a half-full 1 m pipe
    pipe.full_area          # 0.7854 m²
    pipe.is_open            # False

----

Units are explicit
==================

``units`` is keyword-only and **required**. There is no default, because a
cross-section carries no record of the units its numbers were written in — a
silently assumed unit system is a silently wrong answer.

============  ==================  ============================
``units``     Lengths / areas     ``critical_depth()`` flow
============  ==================  ============================
``"US"``      ft, ft²             CFS
``"SI"``      m, m²               CMS
============  ==================  ============================

Section factors (A·R^(2/3)) come back in length\ :sup:`8/3`. A section taken
from a link inherits its model's units, including its actual flow units, which
may be any of CFS/GPM/MGD/CMS/LPS/MLD:

.. code-block:: python

    xs = s.links["C1"].xsect.geometry()
    xs.units          # 'US'
    xs.flow_units     # 'GPM', if that is what the model uses

----

Scalars or arrays
=================

Every query takes a float **or** a NumPy array. Array input runs as a single
batched C call and returns an ``ndarray`` of the same shape, which is how you
build a rating curve without a Python loop:

.. code-block:: python

    import numpy as np

    pipe = XSectionGeometry(XSectShape.CIRCULAR, 2.0, units="US")
    depths = np.linspace(0.0, 2.0, 200)

    area = pipe.area(depths)
    hrad = pipe.hyd_radius(depths)

    # Manning's equation over the whole curve at once.
    n, slope = 0.013, 0.005
    q = (1.49 / n) * area * hrad ** (2 / 3) * slope ** 0.5

    import matplotlib.pyplot as plt
    plt.plot(q, depths)
    plt.xlabel("Discharge (cfs)")
    plt.ylabel("Depth (ft)")

----

Queries
=======

======================================  =========================================
Method                                  Returns
======================================  =========================================
``area(depth)``                         Flow area
``width(depth)``                        Top width of the water surface
``hyd_radius(depth)``                   Hydraulic radius (area / wetted perimeter)
``depth_from_area(area)``               Depth — the inverse of ``area()``
``hyd_radius_from_area(area)``          Hydraulic radius from area
``section_factor(area)``                Section factor, A·R^(2/3)
``area_from_section_factor(sf)``        Area — used to solve for normal depth
``dsda(area)``                          dS/dA
``critical_depth(flow)``                Critical depth
======================================  =========================================

And the full-flow properties, as attributes: ``full_depth``, ``full_area``,
``full_hyd_radius``, ``max_width``, ``full_section_factor``, ``max_area``,
``is_open``.

Depths above the full depth of a closed shape are **clamped**, not rejected —
the routing solvers behave the same way, so a surcharged conduit does not raise.
Negative or non-finite inputs do raise :class:`ValueError`.

----

Building a section
==================

Self-contained shapes
---------------------

Pass the shape and its ``[XSECTIONS]`` Geom1–Geom4 values:

.. code-block:: python

    XSectionGeometry(XSectShape.CIRCULAR, 1.0, units="SI")
    XSectionGeometry(XSectShape.RECT_CLOSED, 3.0, 5.0, units="US")
    XSectionGeometry(XSectShape.TRAPEZOIDAL, 4.0, 2.0, 1.0, 3.0, units="US")
    #                                        depth bot  m1   m2

Two shapes are worth calling out:

* ``FORCE_MAIN`` is geometrically a circular pipe — it differs only in its
  friction law, so its geometric queries match ``CIRCULAR`` of the same
  diameter.
* ``DUMMY`` has no geometry: it constructs, and every query returns 0.

Irregular (natural) channels
----------------------------

Mirrors a ``[TRANSECTS]`` entry. The Manning's *n* values are not decoration —
the hydraulic-radius table is conveyance-weighted across the left overbank,
channel and right overbank, so they change what ``hyd_radius()`` reports:

.. code-block:: python

    creek = XSectionGeometry.from_transect(
        stations=[0.0, 4.0, 6.0, 10.0],
        elevations=[4.0, 0.0, 0.0, 4.0],
        left_bank=4.0, right_bank=6.0,
        n_channel=0.03, n_left=0.06, n_right=0.06,
        units="US",
    )
    creek.full_depth    # 4.0
    creek.max_width     # 10.0
    creek.is_open       # True

Custom shape curves and streets
-------------------------------

.. code-block:: python

    # A SHAPE-type [CURVES] entry, scaled to a 5 ft full depth.
    XSectionGeometry.from_curve(5.0, [0.0, 0.5, 1.0], [0.2, 1.0, 0.6],
                                units="US")

    # A [STREETS] entry. Slopes are percentages, as in the input file.
    XSectionGeometry.from_street(width=20.0, curb_height=0.5, slope=2.0,
                                 roughness=0.016, sides=2, units="US")

A street's full depth is whichever of the curb or the crown stands higher above
the gutter — with a 2% slope over 20 ft the crown rises 0.4 ft, so the 0.5 ft
curb governs.

----

From a link
===========

``link.xsect.geometry()`` returns the geometry the engine **actually built** for
that link, including any transect tables — not a re-derivation from the raw
Geom1–Geom4. The result deep-copies its geometry, so it stays valid after the
solver closes:

.. code-block:: python

    from openswmm.engine import Solver

    with Solver("model.inp") as s:
        xs = s.links["C1"].xsect.geometry()
        capacity = xs.full_area * 3.0        # at 3 ft/s

    xs.area(0.5)   # still works — the solver is gone

.. note::

   This needs resolved geometry. A model still under programmatic construction
   has only its raw geoms stored, so ``geometry()`` raises ``LifecycleError``
   until :meth:`ModelBuilder.finalize` (or ``Solver.open``) has run.

----

Labelled parameters
===================

:meth:`Links.get_xsect_info` names the geometry parameters for you, rather than
leaving you to remember what Geom3 means for a given shape:

.. code-block:: python

    xs = s.links.get_xsect_info("C1")
    xs.shape_name     # 'CIRCULAR'
    xs.geom_labels    # {'diameter': 1.2}

    s.links.get_xsect_info("CH1").geom_labels
    # {'height': 4.0, 'bottom_width': 2.0, 'left_slope': 1.0, 'right_slope': 3.0}

----

.. warning::

   **Shape codes were renumbered in 6.0.** ``XSectShape.IRREGULAR``,
   ``CUSTOM`` and ``FORCE_MAIN`` previously carried the values 16/17/18, which
   the engine read as ``RECT_TRIANG``/``RECT_ROUND``/``HORIZ_ELLIPSE`` —
   assigning them silently produced the wrong cross-section. Seven shapes that
   the enum had been missing (``RECT_TRIANG``, ``RECT_ROUND``,
   ``HORIZ_ELLIPSE``, ``VERT_ELLIPSE``, ``ARCH``, ``STREET_XSECT``, ``DUMMY``)
   were added at the same time, so :class:`XSectShape` now covers all 26.

   Always pass the enum member rather than a bare integer and the correction is
   automatic. :func:`shape_name` resolves a code at runtime if you need to check
   stored data.

.. seealso::

   :doc:`links` for the rest of the link surface, and
   :class:`XSectShape` for the per-shape meaning of Geom1–Geom4.
