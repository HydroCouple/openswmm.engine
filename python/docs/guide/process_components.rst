==============================================
Process components  (``[PROCESS_COMPONENTS]``)
==============================================

.. note::

   **Engine:** OpenSWMM 6 — refactored.

.. currentmodule:: openswmm.engine

``solver.process_components`` enumerates and edits the
``[PROCESS_COMPONENTS]`` registrations — the file-binding surface that
tells the engine which optional process components (heat, water age,
reactions, …) are active and where each one's config file lives.

Reference: ``openswmm_process_components.h``.

----

Quickstart
==========

.. code-block:: python

    from openswmm.engine import Solver

    with Solver("model.inp") as s:
        for c in s.process_components:
            print(c.component_index, c.id, c.config, c.resolved)

        "heat" in s.process_components          # membership by id
        s.process_components["heat"].config     # config="…" as written

        # Register first, then write the file it points at.
        s.process_components.register("waterage", "model.age")
        open("model.age", "w").write("[WATER_AGE_SOURCES]\n")

        s.process_components.remove("waterage")

.. warning::

   ``register`` and ``remove`` are **BUILDING/OPENED only**.

----

The registration record
=======================

Each entry is a :class:`ProcessComponent` named tuple.

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - Field
     - Meaning
   * - ``component_index``
     - Position in the section, ``0 .. len-1``.
   * - ``id``
     - Component id, e.g. ``"heat"``.
   * - ``config``
     - The ``config="…"`` argument **exactly as written**. May be ``""``
       when the registration named no file.
   * - ``resolved``
     - The effective path the config was actually **read from** at the
       last open.

.. important::

   ``resolved`` is ``""`` until the last open resolved it. An empty
   ``resolved`` therefore means "not resolved yet", not "no file" — check
   ``config`` to tell the two apart. On a model built programmatically
   and never opened, every ``resolved`` is empty.

----

Enumerating and finding
=======================

.. list-table::
   :header-rows: 1
   :widths: 44 56

   * - Operation
     - Behaviour
   * - ``len(s.process_components)``
     - Number of registrations.
   * - ``s.process_components[key]``
     - One :class:`ProcessComponent` by ``int`` index or ``str`` id.
   * - ``for c in s.process_components:``
     - Iterate in section order.
   * - ``"heat" in s.process_components``
     - Membership test on the component id.
   * - ``.get_index(component_id)``
     - Registration index for an id.
   * - ``.register(component_id, config_path="")``
     - Add a registration; returns the new
       :class:`ProcessComponent`.
   * - ``.remove(key)``
     - Delete by index or id.

----

Registering before the file exists
==================================

The "create a component and its config file" flow **registers first,
then writes the file**. A ``config_path`` that does not exist yet is
legal: the path is stored as written, and resolution happens at the next
open.

.. code-block:: python

    c = s.process_components.register("reactions", "chlorine.rxn")
    assert c.resolved == ""              # nothing resolved it yet
    s.reactions.save("chlorine.rxn")     # now the file exists

.. warning::

   * A **duplicate id is refused.** Check with ``in`` or
     :meth:`ProcessComponents.get_index` before registering, rather than
     relying on a retry.
   * :meth:`ProcessComponents.remove` **shifts** the indices of every
     later registration. Cached ``component_index`` values go stale — re-enumerate,
     or address components by id.

----

See also
========

* :doc:`heat` — the ``model.heat`` component.
* :doc:`water_age` — the ``model.age`` component, and
  :meth:`WaterAge.save`.
* :doc:`reactions` — the ``.rxn`` component, and :meth:`Reactions.save`.
* :doc:`model_builder` — building a model that registers components from
  scratch.
* :doc:`error_handling` — what a duplicate id or a bad index raises.
