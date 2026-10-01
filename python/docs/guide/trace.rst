Offline flow tracing
====================

``FlowTracer`` estimates upstream or downstream flow paths and travel times
from completed hydraulic output or supplied averages. It owns its topology,
works independently of a live solver, and closes through a ``with`` block.
All dimensional inputs and results use SI units, even when the source output
uses US units.

Analytical example
------------------

.. code-block:: python

   from openswmm.engine import (
       FlowTracer, TraceNode, TraceLink, TraceNodeAverage, TraceLinkAverage,
       TraceDirection, TraceTerminal, NodeType,
   )

   nodes = [TraceNode("S"), TraceNode("O", NodeType.OUTFALL)]
   links = [TraceLink("SO", from_node=0, to_node=1, length_m=30)]
   with FlowTracer(nodes, links) as trace:
       trace.set_averages(
           [TraceNodeAverage(lateral_in_m3s=1), TraceNodeAverage()],
           [TraceLinkAverage(net_flow_m3s=1, absolute_flow_m3s=1,
                             absolute_velocity_mps=1)],
       )
       downstream = trace.estimate("S")
       assert downstream.nodes[1].time_s == 30
       assert downstream.summary.terminal[TraceTerminal.OUTFALL] == 1
       upstream = trace.estimate("O", TraceDirection.UPSTREAM)

Results are immutable snapshots in topology order and remain valid after the
handle closes. ``trace.node_ids``, ``trace.link_ids``, and ``trace.topology``
describe that order. ``trace.averages`` returns node/link snapshots that can be
restored with ``set_averages(..., info=trace.info)``. Native numerical defaults
are available through ``FlowTracer.default_options()`` and can be adjusted
with ``dataclasses.replace`` before passing them as ``options=``.

Completed output and caching
----------------------------

For a real model, supply every node and link with matching IDs, node/link
types, physical link lengths, endpoints, and appropriate ``TraceNodeFlags``.
The reader matches output records by ID, regardless of their row order.

.. code-block:: python

   with FlowTracer(nodes, links) as trace:
       trace.prepare("completed.out")
       result = trace.estimate("S")

An optional HDF5 cache requires a verified content fingerprint. For example,
compute SHA-256 from the completed output bytes and pass its hexadecimal
digest as ``fingerprint=`` alongside ``cache_path="completed.trace.h5"``.
Recompute the digest whenever the output changes; a path or timestamp is not
a content fingerprint. Caching reports ``TraceStatus.NO_HDF5`` when the
engine was built without HDF5; uncached tracing remains available.

Interpreting results and errors
-------------------------------

``TraceResult`` contains ``nodes``, ``links``, and ``summary``. Passage ratios
may exceed one where flow recirculates. Times are estimates under frozen
average flows, not particle arrival times. Unknown times remain NaN;
``time_coverage`` and ``TraceFlags`` distinguish partial timing, reversal,
unreachable elements, and trapped circulation. Index
``summary.terminal`` with ``TraceTerminal`` for terminal accounting.
``terminal_kind`` is ``None`` for results without a terminal classification.

``prepare`` and ``estimate`` accept ``progress(fraction, stage)``. Returning
true cancels and raises ``TraceError`` with ``code == TraceStatus.CANCELLED``.
A callback exception is re-raised after the native operation stops. Long
native operations release the GIL, but each handle permits only one operation
at a time: concurrent or callback access to that handle raises ``RuntimeError``.
Use separate handles for independent analyses. Native tracing failures raise
``TraceError`` with a ``TraceStatus`` code; closed handles raise ``RuntimeError``.
