=============
Control rules
=============

.. note::

   **Engine:** OpenSWMM 6 — refactored.  Documents
   :class:`openswmm.engine.Controls`.

.. currentmodule:: openswmm.engine

The :class:`Controls` class manages the SWMM rule-based control
system.  Rules use the same syntax as the ``[CONTROLS]`` section in
the ``.inp`` file:

.. code-block:: text

    RULE  R1
    IF    NODE WET_WELL DEPTH >= 4.0
    THEN  PUMP P1 STATUS = ON
    ELSE  PUMP P1 STATUS = OFF
    PRIORITY 1

You can add, list, replace, or delete rules at runtime, and you can
also bypass the rule engine and force a link setting / status
imperatively.

For Python-driven control where the trigger is computed externally
(an ML controller, a custom scheduler), prefer :class:`Forcing` —
see :doc:`forcing`.

Reference: ``openswmm_controls.h``.

----

Class signature
===============

.. code-block:: python

    class Controls:
        def __init__(self, solver: Solver) -> None: ...

----

Key methods
===========

Rule management
---------------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - Action / returns
   * - :meth:`count()`
     - Number of currently active rules.
   * - :meth:`get_rule(idx)`
     - Retrieve the source text of rule ``idx``.
   * - :meth:`add_rule(text)`
     - Append a rule; ``text`` uses the standard SWMM rule syntax.
   * - :meth:`clear_rules()`
     - Remove all rules.

Imperative control  (bypass the rule engine)
--------------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - Action
   * - :meth:`set_link_setting(link_idx, setting)`
     - Force a control setting (0–1) on a link.
   * - :meth:`set_link_status(link_idx, status)`
     - Force a binary open/close on a link.

These imperative setters fire **once** at the time of the call.  On
the next step, any active rule that targets the same link will
override them.  For per-step persistence use :class:`Forcing`.

----

End-to-end example
==================

.. code-block:: python

    from openswmm.engine import Solver, Controls, Links

    with Solver("model.inp", "model.rpt", "model.out") as s:
        controls = Controls(s)
        links = Links(s)

        # Register a runtime rule before initialize()
        s.open()
        controls.add_rule(
            "RULE FILL_TANK\n"
            "IF NODE WET_WELL DEPTH >= 4.0\n"
            "THEN PUMP P1 STATUS = ON\n"
            "ELSE PUMP P1 STATUS = OFF\n"
            "PRIORITY 5\n"
        )
        s.initialize()
        s.start()

        p1 = links.get_index("P1")
        on_steps = 0
        while s.step():
            if links.get_target_setting(p1) > 0.5:
                on_steps += 1
        print(f"P1 was on for {on_steps} steps "
              f"(rules engaged so the pump cycled).")

----

Common recipes
==============

Add a rule at runtime  (mid-simulation)
---------------------------------------

.. code-block:: python

    while s.step():
        if some_external_condition():
            controls.add_rule(
                "RULE EMERGENCY_BYPASS\n"
                "IF NODE J5 DEPTH >= 5.0\n"
                "THEN ORIFICE OR1 SETTING = 1.0\n"
                "PRIORITY 100\n"
            )

Higher ``PRIORITY`` wins when multiple rules fire simultaneously.

Inspect the active rule set
---------------------------

.. code-block:: python

    print(f"{controls.count()} rules active")
    for i in range(controls.count()):
        print(f"--- rule {i} ---")
        print(controls.get_rule(i))

Replace the rule set wholesale
------------------------------

.. code-block:: python

    controls.clear_rules()
    for rule_text in my_new_rule_list:
        controls.add_rule(rule_text)

Imperative override  (one step)
-------------------------------

.. code-block:: python

    p1 = links.get_index("P1")

    while s.step():
        if external_signal_says_pump_off():
            controls.set_link_setting(p1, 0.0)        # one-shot

For persistent imperative control, use :class:`Forcing.link_setting`.

----

EngineState requirements & exceptions
=====================================

.. list-table::
   :header-rows: 1
   :widths: 30 25 45

   * - Method group
     - Required state
     - Notes
   * - rule add / clear
     - ``OPENED`` or later
     - Rules added at ``OPENED`` are evaluated from the first step.
   * - ``count`` / ``get_rule``
     - any state where the solver is alive
     - n/a
   * - imperative ``set_link_*``
     - ``RUNNING``
     - One-shot per call.

Common :class:`EngineError` codes:

* ``INVALID_INDEX``  — link index out of range.
* Parsing errors on ``add_rule`` raise with the rule text echoed in
  the message.

----

See also
========

* :doc:`forcing` — the right tool for persistent Python-driven
  control without writing rule text.
* :doc:`links` — observe the resulting setting / status.
