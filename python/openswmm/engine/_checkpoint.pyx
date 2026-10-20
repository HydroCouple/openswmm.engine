# SPDX-License-Identifier: Apache-2.0
"""Managed checkpoint sessions and immutable inspection metadata."""
import os
import json
from libc.stdlib cimport malloc, free
from ._common cimport SWMM_Engine
from ._solver cimport Solver
from ._access import EngineView
from ._exceptions import raise_for_code

cdef extern from "openswmm/engine/openswmm_checkpoint.h":
    ctypedef void* SWMM_CheckpointInspection
    ctypedef struct SWMM_CheckpointSessionOptions:
        size_t struct_size
        unsigned long schema_version
        int output
        const char* source_history
        int detailed_source_intervals
        const char* ownership_token
    int swmm_checkpoint_capabilities(SWMM_Engine,unsigned long*)
    int swmm_checkpoint_session_status(SWMM_Engine,unsigned long*)
    int swmm_checkpoint_start_session(SWMM_Engine,const SWMM_CheckpointSessionOptions*) nogil
    int swmm_checkpoint_inspect(SWMM_Engine,const char*,SWMM_CheckpointInspection*) nogil
    int swmm_checkpoint_inspection_json(SWMM_CheckpointInspection,char*,size_t,size_t*)
    void swmm_checkpoint_inspection_destroy(SWMM_CheckpointInspection)
    int swmm_checkpoint_save(SWMM_Engine,const char*) nogil
    int swmm_checkpoint_restore(SWMM_Engine,const char*) nogil
    int swmm_checkpoint_export_bundle(const char*,const char*) nogil
    const char* swmm_checkpoint_last_error_msg()

cdef extern from "openswmm/engine/openswmm_engine.h":
    int swmm_engine_get_elapsed_seconds(SWMM_Engine,double*)

cdef bytes _text(value):
    result=os.fspath(value).encode('utf-8')
    if b'\0' in result:raise ValueError('Checkpoint path/token contains NUL')
    return result

cdef void _check(int code) except *:
    if code:
        message=swmm_checkpoint_last_error_msg()
        raise_for_code(code, message.decode('utf-8', 'replace') if message!=NULL else None)

class Checkpoints(EngineView):
    """``solver.checkpoints``; native diagnostics and exclusive owner access.

    Calls require the engine's owning thread. Inspection returns schema-1 JSON
    metadata with raw OLE dates and decimal-string uint64 values. Restore checks
    compatibility again. Save/export require fresh destinations and preserve
    older files. Supported profiles include native water-only/1D reactions and
    mesh-only matched BULK quality with optional age and either WATER_ONLY or
    paired porous-ground heat.
    External drivers and broader coupled owners retain native profile guards.
    """
    @property
    def capabilities(self):
        """Native support bitmask (bits 0..8 as in openswmm_checkpoint.h)."""
        cdef unsigned long flags
        _check(swmm_checkpoint_capabilities(<SWMM_Engine><size_t>self._address(),&flags))
        return flags

    @property
    def session_status(self):
        """Bitmask: attached=1, boundary=2, fresh=4, managed=8."""
        cdef unsigned long flags
        _check(swmm_checkpoint_session_status(<SWMM_Engine><size_t>self._address(),&flags))
        return flags

    def start_session(self, output=0, *, source_history=None,
                      detailed_source_intervals=False, ownership_token=None):
        """Initialize/start from OPENED, before ordinary initialize.

        Output: 0 none, 1 binary, 2 mesh, 3 combined, 4 files. Set fresh output
        slots before calling; profiles without binary output need an empty out
        path. Post-initialization failure closes the engine and can leave partial
        fresh outputs in the caller's workspace. Preflight refusal keeps OPENED.
        """
        cdef Solver owner=self._owner
        owner._access.require_idle()
        cdef SWMM_Engine h=<SWMM_Engine><size_t>self._address()
        cdef bytes history=_text(source_history) if source_history is not None else b''
        cdef bytes token=_text(ownership_token) if ownership_token is not None else b''
        cdef SWMM_CheckpointSessionOptions options
        options.struct_size=sizeof(options)
        options.schema_version=1
        options.output=output
        options.source_history=history
        options.detailed_source_intervals=bool(detailed_source_intervals)
        options.ownership_token=token
        cdef int code
        with owner._operation(<size_t>h):
            with nogil:code=swmm_checkpoint_start_session(h,&options)
            _check(code)
            owner._elapsed=0.0

    def inspect(self, path):
        """Validate privately and return metadata, without adopting any state."""
        cdef Solver owner=self._owner
        owner._access.require_idle()
        cdef SWMM_Engine h=<SWMM_Engine><size_t>self._address()
        cdef bytes encoded=_text(path)
        cdef const char* filename=encoded
        cdef SWMM_CheckpointInspection snapshot=NULL
        cdef size_t size=0
        cdef char* buffer=NULL
        cdef int code
        with self._owner._operation(<size_t>h):
            try:
                with nogil:code=swmm_checkpoint_inspect(h,filename,&snapshot)
                _check(code)
                _check(swmm_checkpoint_inspection_json(snapshot,NULL,0,&size))
                buffer=<char*>malloc(size)
                if buffer==NULL:raise MemoryError()
                _check(swmm_checkpoint_inspection_json(snapshot,buffer,size,&size))
                return json.loads(buffer.decode('utf-8'))
            finally:
                free(buffer)
                if snapshot!=NULL:swmm_checkpoint_inspection_destroy(snapshot)

    def save(self, path):
        """Save at an accepted boundary to a new exclusive destination."""
        cdef Solver owner=self._owner
        owner._access.require_idle()
        cdef SWMM_Engine h=<SWMM_Engine><size_t>self._address()
        cdef bytes encoded=_text(path)
        cdef const char* filename=encoded
        cdef int code
        with self._owner._operation(<size_t>h):
            with nogil:code=swmm_checkpoint_save(h,filename)
            _check(code)

    def restore(self, path):
        """Restore a matching checkpoint into a prepared fresh session."""
        cdef Solver owner=self._owner
        owner._access.require_idle()
        cdef SWMM_Engine h=<SWMM_Engine><size_t>self._address()
        cdef bytes encoded=_text(path)
        cdef const char* filename=encoded
        cdef double seconds
        cdef int code
        with self._owner._operation(<size_t>h):
            with nogil:code=swmm_checkpoint_restore(h,filename)
            _check(code)
            _check(swmm_engine_get_elapsed_seconds(h,&seconds))
            owner._elapsed=seconds/86400.0

    @staticmethod
    def export_bundle(path, new_folder):
        """Copy a file-backed checkpoint to a new exclusive folder.

        External model inputs remain required; export is not certification of
        model/provider compatibility.
        """
        cdef bytes source=_text(path), destination=_text(new_folder)
        cdef const char* filename=source
        cdef const char* folder=destination
        cdef int code
        with nogil:code=swmm_checkpoint_export_bundle(filename,folder)
        _check(code)
