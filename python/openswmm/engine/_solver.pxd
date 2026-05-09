# :author: Caleb Buahin
# :copyright: Copyright (c) HydroCouple 2026
# :license: MIT
#
# _solver.pxd -- Expose Solver cdef class for cimport by _model and _hotstart.
# cython: language_level=3

from ._common cimport SWMM_Engine

cdef class Solver:
    cdef SWMM_Engine _handle
    cdef str _inp, _rpt, _out
    cdef double _elapsed
    cdef object _step_begin_cb
    cdef object _step_end_cb
