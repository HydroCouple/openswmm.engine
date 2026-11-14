# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Caleb Buahin
"""Independent native reader for saved 2D HDF5 results (no h5py dependency)."""
import os
from threading import RLock
import numpy as np
cimport numpy as np
from libc.stdint cimport uint64_t, int64_t
from libcpp.vector cimport vector
from ._enums import ErrorCode
from ._exceptions import BadHandleError, FileError, raise_for_code

cdef extern from "openswmm/engine/openswmm_output2d.h" nogil:
    ctypedef void* SWMM_Output2D
    int swmm_output2d_available()
    int swmm_output2d_open(const char*, SWMM_Output2D*)
    void swmm_output2d_close(SWMM_Output2D)
    const char* swmm_output2d_error()
    int swmm_output2d_datasets(SWMM_Output2D, char*, size_t, size_t*)
    int swmm_output2d_shape(SWMM_Output2D, const char*, int*, uint64_t*, int*, uint64_t*)
    int swmm_output2d_attributes(SWMM_Output2D, const char*, char*, size_t, size_t*)
    int swmm_output2d_attribute(SWMM_Output2D, const char*, const char*, char*, size_t, size_t*)
    int swmm_output2d_strings(SWMM_Output2D, const char*, char*, size_t, size_t*)
    int swmm_output2d_read_double(SWMM_Output2D, const char*, const uint64_t*,
        const uint64_t*, const uint64_t*, double*, size_t)
    int swmm_output2d_read_int64(SWMM_Output2D, const char*, const uint64_t*,
        const uint64_t*, const uint64_t*, int64_t*, size_t)

cdef void check(int status) except *:
    if status:
        raise_for_code(status, (<bytes>swmm_output2d_error()).decode('utf-8', 'replace'))

cdef class Output2DReader:
    """Read a closed 2D result file or immutable copy independently of a solver.

    Paths accept os.PathLike. Dataset selections use normal Python integers and
    slices, in native dimension order. Numeric arrays own their
    memory and remain usable after close. Concurrent operations are serialized.
    """
    cdef SWMM_Output2D _handle
    cdef object _lock
    cdef object _path

    def __init__(self, path):
        self._lock = RLock()
        self._path = os.fspath(path)
        cdef bytes encoded = os.fsencode(path)
        if b'\0' in encoded:
            raise ValueError('Embedded NUL in output path')
        cdef int status = swmm_output2d_open(encoded, &self._handle)
        if status == ErrorCode.IO or status == ErrorCode.BADINDEX:
            raise FileError(ErrorCode.OUTFILE, (<bytes>swmm_output2d_error()).decode('utf-8', 'replace'))
        check(status)

    @staticmethod
    def available():
        """Whether the linked engine supports HDF5 result reading."""
        return bool(swmm_output2d_available())

    cdef SWMM_Output2D _h(self) except NULL:
        if self._handle == NULL:
            raise BadHandleError(ErrorCode.BADHANDLE, 'Output2DReader is closed')
        return self._handle

    def close(self):
        """Release the file. Repeated close calls are harmless."""
        with self._lock:
            swmm_output2d_close(self._handle)
            self._handle = NULL

    def __dealloc__(self):
        swmm_output2d_close(self._handle)

    def __enter__(self):
        self._h()
        return self

    def __exit__(self, *args):
        self.close()
        return False

    @property
    def path(self):
        return self._path

    cdef str _text(self, int kind, bytes path=b'/', bytes name=b''):
        cdef size_t required = 0
        cdef vector[char] buffer
        cdef SWMM_Output2D h = self._h()
        if kind == 0:
            check(swmm_output2d_datasets(h, NULL, 0, &required))
        elif kind == 1:
            check(swmm_output2d_attributes(h, path, NULL, 0, &required))
        elif kind == 2:
            check(swmm_output2d_attribute(h, path, name, NULL, 0, &required))
        else:
            check(swmm_output2d_strings(h, path, NULL, 0, &required))
        buffer.resize(required)
        if kind == 0:
            check(swmm_output2d_datasets(h, buffer.data(), required, &required))
        elif kind == 1:
            check(swmm_output2d_attributes(h, path, buffer.data(), required, &required))
        elif kind == 2:
            check(swmm_output2d_attribute(h, path, name, buffer.data(), required, &required))
        else:
            check(swmm_output2d_strings(h, path, buffer.data(), required, &required))
        return (<bytes>buffer.data()).decode('utf-8')

    @property
    def datasets(self):
        """Absolute paths of numeric/string datasets, including diagnostic groups."""
        with self._lock:
            return tuple(self._text(0).splitlines())

    def attributes(self, path='/'):
        """Attribute mapping. Native numeric attributes are represented as text."""
        cdef bytes encoded = str(path).encode('utf-8')
        with self._lock:
            names = self._text(1, encoded).splitlines()
            return {name: self._text(2, encoded, name.encode('utf-8')) for name in names}

    def shape(self, path):
        """Return (shape, output dtype); numeric reads use float64 or int64."""
        return self._describe(path)[:2]

    def unlimited_axes(self, path):
        """Indices of appendable dataset axes (as declared in the file)."""
        return self._describe(path)[2]

    def strings(self, path):
        """Read a small string identity table, preserving its native order."""
        with self._lock:
            return tuple(self._text(3, str(path).encode('utf-8')).split('\n')[:-1])

    cdef tuple _describe(self, path):
        cdef uint64_t dimensions[8]
        cdef uint64_t unlimited = 0
        cdef int rank = 0, integer = 0
        cdef bytes encoded = str(path).encode('utf-8')
        with self._lock:
            check(swmm_output2d_shape(self._h(), encoded, &rank, dimensions, &integer, &unlimited))
        return (tuple(dimensions[i] for i in range(rank)),
                np.dtype('str' if integer == -1 else 'int64' if integer else 'float64'),
                tuple(i for i in range(rank) if unlimited & (<uint64_t>1 << i)))

    def read(self, path, selection=None):
        """Read a bounded selection; integer axes are removed from the result.

        A single selector applies to axis zero. Ellipsis fills remaining axes.
        Higher-level fields support ordered object lists without loading
        unselected objects. Numeric reads accept integer/slice/ellipsis axes;
        negative slices reverse bounded native reads.
        """
        import operator
        cdef uint64_t start[8]
        cdef uint64_t count[8]
        cdef uint64_t stride[8]
        cdef bytes encoded = str(path).encode('utf-8')
        cdef const char* dataset = encoded
        cdef int status
        cdef np.ndarray values
        cdef double* doubles
        cdef int64_t* integers
        cdef size_t capacity
        cdef SWMM_Output2D h
        with self._lock:
            h = self._h()
            shape, dtype = self.shape(path)
            if dtype.kind == 'U':
                values = np.asarray(self.strings(path), dtype=str)
                return values if selection is None else values[selection]
            keys = () if selection is None else selection if isinstance(selection, tuple) else (selection,)
            if sum(key is Ellipsis for key in keys) > 1:
                raise IndexError('Only one ellipsis is allowed')
            if any(key is Ellipsis for key in keys):
                at = next(i for i, key in enumerate(keys) if key is Ellipsis)
                keys = keys[:at] + (slice(None),) * (len(shape) - len(keys) + 1) + keys[at + 1:]
            if len(keys) > len(shape):
                raise IndexError('Too many selection axes')
            keys += (slice(None),) * (len(shape) - len(keys))
            kept, extents, reversed_axes = [], [], []
            for i, (key, length) in enumerate(zip(keys, shape)):
                if isinstance(key, slice):
                    lo, hi, step = key.indices(length)
                    n = len(range(lo, hi, step))
                    if step < 0:
                        reversed_axes.append(len(kept))
                        lo = lo + (n - 1) * step if n else 0
                        step = -step
                    kept.append(n)
                else:
                    lo = operator.index(key)
                    if lo < 0:
                        lo += length
                    if lo < 0 or lo >= length:
                        raise IndexError('Result index out of range')
                    n, step = 1, 1
                start[i], count[i], stride[i] = lo, n, step
                extents.append(n)
            values = np.empty(tuple(extents), dtype=dtype)
            capacity = values.size
            if dtype.kind == 'i':
                integers = <int64_t*>values.data
                with nogil:
                    status = swmm_output2d_read_int64(h, dataset, start, count, stride, integers, capacity)
            else:
                doubles = <double*>values.data
                with nogil:
                    status = swmm_output2d_read_double(h, dataset, start, count, stride, doubles, capacity)
            check(status)
            result = values.reshape(tuple(kept))
            return np.flip(result, axis=tuple(reversed_axes)) if reversed_axes else result
