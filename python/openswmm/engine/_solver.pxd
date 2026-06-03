# :author: Caleb Buahin
# :copyright: Copyright (c) 2026 Caleb Buahin
# :license: MIT
#
# _solver.pxd -- Expose Solver cdef class for cimport by _model and _hotstart.
# cython: language_level=3

from ._common cimport SWMM_Engine

cdef class Solver:
    cdef SWMM_Engine _handle
    cdef str _inp, _rpt, _out
    cdef object _plugin_lib  # str or None
    cdef double _elapsed
    cdef object _step_begin_cb
    cdef object _step_end_cb
    cdef object _warning_cb
    cdef object _progress_cb
    # P1 — lazy views & domain-collection accessors. Cached on first access so
    # ``solver.options is solver.options`` holds for a given Solver instance.
    # ``_generation`` is bumped on structural mutations (add/delete/rename) so
    # wrappers handed out in P2+ can detect staleness.
    cdef object _options
    cdef object _userflags
    cdef object _events_view
    cdef object _nodes
    cdef object _links
    cdef object _subcatchments
    cdef object _aquifers
    cdef object _snowpacks
    cdef object _gages
    cdef object _pollutants
    cdef object _tables
    cdef object _patterns
    cdef object _inflows
    cdef object _controls
    cdef object _forcing
    cdef object _infrastructure
    cdef object _spatial
    cdef object _quality
    cdef object _statistics
    cdef object _mass_balance
    cdef object _editor
    cdef long long _generation
