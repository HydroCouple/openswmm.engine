# SPDX-License-Identifier: Apache-2.0
"""Experimental native water-only peers. Frames own immutable native snapshots.

Configure each OPENED peer before initializing, export/apply geometry, register
one network callback, then start both peers. Callback requests expire on return;
return a newly owned reply from the surface peer's evaluate method. There is no
rollback after a failed phase. Native eligibility and feature checks remain authoritative.
"""
from contextlib import contextmanager
from threading import RLock, get_ident
from libc.stdint cimport uint64_t
from ._common cimport SWMM_Engine, _check
from ._solver cimport Solver
from ._access import EngineView
from ._enums import SplitRole, SplitPhase

cdef extern from "openswmm/engine/openswmm_split.h":
    ctypedef void* SWMM_SplitFrame
    ctypedef int (*SWMM_SplitCallback)(SWMM_Engine, int, SWMM_SplitFrame, SWMM_SplitFrame*, void*) noexcept
    int swmm_split_configure(SWMM_Engine, int)
    int swmm_split_geometry(SWMM_Engine, SWMM_SplitFrame*)
    int swmm_split_apply_geometry(SWMM_Engine, SWMM_SplitFrame)
    int swmm_split_set_callback(SWMM_Engine, SWMM_SplitCallback, void*)
    int swmm_split_evaluate(SWMM_Engine, int, SWMM_SplitFrame, SWMM_SplitFrame*) nogil
    void swmm_split_frame_destroy(SWMM_SplitFrame)
    int swmm_split_frame_node(SWMM_SplitFrame, int, double*, double*, double*, double*, double*)
    int swmm_split_frame_time(SWMM_SplitFrame, uint64_t*, double*, double*)
    int swmm_split_frame_source(SWMM_SplitFrame, int, double*, double*, double*, double*)
    int swmm_split_frame_return(SWMM_SplitFrame, int, double*)

cdef extern from "openswmm/engine/openswmm_split_packet.h":
    int swmm_split_validate_input(SWMM_Engine)
    int swmm_split_packet_encode(SWMM_Engine, SWMM_SplitFrame, char**)
    int swmm_split_packet_decode(SWMM_Engine, const char*, size_t, SWMM_SplitFrame*)
    int swmm_split_packet_phase(SWMM_SplitFrame)
    void swmm_split_packet_free(char*)


cdef class SplitFrame:
    """Owned snapshot, or callback-only borrowed request. Close owned frames promptly."""
    cdef SWMM_SplitFrame _handle
    cdef bint _owned
    cdef object _lock, _thread

    def __cinit__(self):
        self._handle = NULL
        self._owned = True
        self._lock = RLock()
        self._thread = None

    def __init__(self, Solver solver=None):
        """With a solver, export initialized surface geometry; otherwise create a closed frame."""
        if self._handle != NULL or not self._owned:
            raise ValueError("Cannot reinitialize an open or borrowed split frame")
        if solver is not None:
            with solver._operation():
                _check(swmm_split_geometry(<SWMM_Engine><size_t>solver.handle, &self._handle))

    def __dealloc__(self):
        if self._owned and self._handle != NULL:
            swmm_split_frame_destroy(self._handle)

    cdef void _require(self) except *:
        if self._handle == NULL:
            raise ValueError("Split frame is closed, transferred, or expired")
        if not self._owned and get_ident() != self._thread:
            raise RuntimeError("Borrowed split requests may only be used on the callback thread")

    def close(self):
        """Release an owned frame; borrowed callback requests cannot be closed."""
        if self._handle == NULL:
            return
        with _frame_use(self):
            if not self._owned:
                raise ValueError("Cannot close a borrowed callback request")
            swmm_split_frame_destroy(self._handle)
            self._handle = NULL

    def __enter__(self):
        self._require()
        return self

    def __exit__(self, *args):
        self.close()

    @property
    def phase(self):
        """Native phase code; zero denotes geometry."""
        with _frame_use(self):
            return swmm_split_packet_phase(self._handle)

    @property
    def time(self):
        """Sequence number, elapsed interval start and duration in seconds."""
        cdef uint64_t sequence
        cdef double start, duration
        with _frame_use(self):
            _check(swmm_split_frame_time(self._handle, &sequence, &start, &duration))
        return sequence, start, duration

    def node(self, int index):
        """Owned SI node row: head/depth m, queued/direct m³, delivery seconds."""
        cdef double head, depth, queued, direct, duration
        with _frame_use(self):
            _check(swmm_split_frame_node(self._handle, index, &head, &depth, &queued, &direct, &duration))
        return dict(head_m=head, depth_m=depth, queued_m3=queued, direct_m3=direct, delivery_seconds=duration)

    def source(self, int index):
        """Reviewed source rates in native ft³/s and pending runoff/drain in ft³."""
        cdef double rate, previous, runoff, drain
        with _frame_use(self):
            _check(swmm_split_frame_source(self._handle, index, &rate, &previous, &runoff, &drain))
        return dict(rate_cfs=rate, previous_rate_cfs=previous, pending_runoff_ft3=runoff, pending_drain_ft3=drain)

    def outfall_return(self, int subcatchment):
        """Accepted reviewed-source outfall return, native ft³."""
        cdef double volume
        with _frame_use(self):
            _check(swmm_split_frame_return(self._handle, subcatchment, &volume))
        return volume


@contextmanager
def _frame_use(SplitFrame frame not None):
    if not frame._lock.acquire(blocking=False):
        raise RuntimeError("Split frame is in use by another thread")
    try:
        frame._require()
        yield
    finally:
        frame._lock.release()


cdef SplitFrame _frame(SWMM_SplitFrame handle, bint owned=True):
    cdef SplitFrame result
    try:
        result = SplitFrame()
    except BaseException:
        if owned and handle != NULL:
            swmm_split_frame_destroy(handle)
        raise
    result._handle = handle
    result._owned = owned
    if not owned:
        result._thread = get_ident()
    return result


cdef int _reply_callback(SWMM_Engine engine, int phase, SWMM_SplitFrame request,
                         SWMM_SplitFrame* reply, void* user) noexcept with gil:
    state = <object>user
    cdef SplitFrame borrowed = None
    cdef SplitFrame response
    reply[0] = NULL
    try:
        if state[0].error is not None:
            return -1
        borrowed = _frame(request, False)
        result = state[1](SplitPhase(phase), borrowed)
        if not isinstance(result, SplitFrame):
            raise TypeError("Split callback must return a newly owned SplitFrame")
        response = result
        with _frame_use(response):
            if not response._owned:
                raise ValueError("Cannot return the borrowed request as a reply")
            reply[0] = response._handle
            response._handle = NULL
        return 0
    except BaseException as error:
        if state[0].error is None:
            state[0].error = error
        return -1
    finally:
        if borrowed is not None:
            borrowed._handle = NULL


class SplitParticipant(EngineView):
    """Owner-backed native water peer, exposed as Solver.split."""

    def __init__(self, Solver owner not None):
        super().__init__(owner)

    def validate_input(self):
        """Check authored eligibility in OPENED state without advancing or opening outputs."""
        _check(swmm_split_validate_input(<SWMM_Engine><size_t>self._address()))

    def configure(self, int role):
        """Select SplitRole.NETWORK or SURFACE once while OPENED, before output binding."""
        (<Solver>self._owner)._access.require_idle()
        _check(swmm_split_configure(<SWMM_Engine><size_t>self._address(), role))

    def geometry(self):
        """Export an owned surface geometry frame after initialize, before start."""
        cdef SWMM_SplitFrame result = NULL
        with self._owner._operation():
            _check(swmm_split_geometry(<SWMM_Engine><size_t>self._address(), &result))
        return _frame(result)

    def apply_geometry(self, SplitFrame frame not None):
        """Import matching initialized geometry without consuming the frame."""
        with self._owner._operation(), _frame_use(frame):
            _check(swmm_split_apply_geometry(<SWMM_Engine><size_t>self._address(), frame._handle))

    def set_callback(self, callback):
        """Register callback(phase, request)->owned reply; None unregisters it.

        Register/unregister only in INITIALIZED, before start. The solver retains
        the callback. A borrowed request expires when it returns;
        exceptions stop native peer exchange and are re-raised after the native call.
        """
        cdef Solver owner = self._owner
        owner._access.require_idle()
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._address()
        if callback is None:
            _check(swmm_split_set_callback(h, NULL, NULL))
            owner._split_cb = None
        else:
            if not callable(callback):
                raise TypeError("callback must be callable or None")
            state = (owner._access, callback)
            _check(swmm_split_set_callback(h, _reply_callback, <void*>state))
            owner._split_cb = state

    def evaluate(self, int phase, SplitFrame request not None):
        """Evaluate/advance the surface for a native request; return a newly owned reply."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._address()
        cdef SWMM_SplitFrame result = NULL
        cdef int rc
        with self._owner._operation(), _frame_use(request):
            with nogil:
                rc = swmm_split_evaluate(h, phase, request._handle, &result)
        value = _frame(result)
        (<Solver>self._owner)._access.raise_callback_error()
        _check(rc)
        return value

    def encode(self, SplitFrame frame not None):
        """Copy a frame to versioned UTF-8 JSON; requires the native packet feature."""
        cdef char* text = NULL
        with self._owner._operation(), _frame_use(frame):
            _check(swmm_split_packet_encode(<SWMM_Engine><size_t>self._address(), frame._handle, &text))
            try:
                return text.decode('utf-8')
            finally:
                swmm_split_packet_free(text)

    def decode(self, str text):
        """Decode JSON into an owned frame without advancing the engine."""
        cdef bytes data = text.encode('utf-8')
        if b'\0' in data:
            raise ValueError('Split packet cannot contain null bytes')
        cdef SWMM_SplitFrame result = NULL
        with self._owner._operation():
            _check(swmm_split_packet_decode(<SWMM_Engine><size_t>self._address(), data, len(data), &result))
        return _frame(result)
