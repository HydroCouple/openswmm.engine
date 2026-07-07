=============
Control rules
=============

.. note::

   **Engine:** OpenSWMM 6 — refactored.

.. currentmodule:: openswmm.engine

``solver.controls`` is a :class:`MutableSequence` of :class:`ControlRule`
records over the ``[CONTROLS]`` block, plus runtime link-override
helpers.

Reference: ``openswmm_controls.h``.

----

Quickstart
==========

.. code-block:: python

    from openswmm.engine import Solver

    with Solver("model.inp") as s:
        # Append a rule.
        s.controls.append(\"\"\"
            RULE r1
              IF NODE J1 DEPTH > 1.0
              THEN ORIFICE OR1 SETTING = 0.5
        \"\"\")

        # Iterate / inspect.
        for rule in s.controls:
            print(rule.id, rule.text)

        # Direct runtime actions on a link.
        s.controls.set_link_setting("OR1", 0.25)
        s.controls.set_link_status("OR1", closed=False)

----

:class:`Controls` — MutableSequence
===================================

.. list-table::
   :header-rows: 1
   :widths: 36 64

   * - Operation
     - What it does
   * - ``len(s.controls)`` / ``s.controls[i]`` / ``for r in s.controls:``
     - Container protocol over :class:`ControlRule` records.
   * - ``s.controls.append(text)``
     - Append a rule. Accepts a raw string, a :class:`ControlRule`, or
       ``{"text": ...}``.
   * - ``s.controls.insert(i, text)``
     - Insert in the middle. Emulated by clear + rebuild.
   * - ``del s.controls[i]`` / ``s.controls.clear()``
     - Remove entries.

Direct runtime actions
----------------------

.. list-table::
   :header-rows: 1
   :widths: 36 64

   * - Method
     - What it does
   * - ``set_link_setting(link, setting)``
     - Set the link's control setting. ``link`` accepts ``int | str``.
   * - ``set_link_status(link, *, closed)``
     - Open / close the link.
   * - ``validate_message(rule_text)``
     - Check a rule **without** adding it. Returns ``(ok, message)`` —
       ``ok`` is ``True`` and ``message`` empty when the rule parses,
       otherwise ``False`` with the engine's error description.

Validate a rule before appending it:

.. code-block:: python

    ok, msg = s.controls.validate_message(rule_text)
    if not ok:
        raise ValueError(f"bad rule: {msg}")
    s.controls.append(rule_text)

----

See also
========

* :doc:`links` — :attr:`Link.control_setting` and :attr:`Link.target_setting`.
* :doc:`forcing` — for non-rule-based overrides.
* :doc:`error_handling`.
