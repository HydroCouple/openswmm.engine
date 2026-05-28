"""
Exception hierarchy for :mod:`openswmm.engine`.

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

Every non-zero return from the C API is mapped to an exception subclass
that **also** inherits from a standard Python exception, so callers can
write idiomatic handlers without importing engine-specific symbols:

.. code-block:: python

    try:
        node = solver.nodes["UNKNOWN"]
    except KeyError:                    # also caught by EngineError
        ...

    try:
        node = solver.nodes[10_000]
    except IndexError:                  # also caught by EngineError
        ...

The mapping from ``SWMM_ERR_*`` codes to exception classes lives in
:func:`raise_for_code`, called from the shared ``_check()`` helper in
:mod:`openswmm.engine._common`.
"""

from __future__ import annotations

from typing import Optional

from ._enums import ErrorCode

__all__ = [
    "EngineError",
    "BadHandleError",
    "BadIndexError",
    "BadParamError",
    "LifecycleError",
    "HotStartError",
    "PluginError",
    "FileError",
    "ParseError",
    "NumericalError",
    "CRSError",
    "DependencyError",
    "StaleObjectError",
    "raise_for_code",
]


class EngineError(Exception):
    """Base class for every C API failure surfaced to Python.

    :ivar code: The raw ``SWMM_ERR_*`` integer code returned by the C API.
    :ivar code_enum: The same code as an :class:`ErrorCode` value, or
        :attr:`ErrorCode.INTERNAL` if the integer doesn't map to a known
        enum member.
    :ivar message: Human-readable description, populated from
        ``swmm_error_message(code)`` when no explicit message is given.
    """

    code: int
    code_enum: ErrorCode
    message: str

    def __init__(self, code: int, message: str = "") -> None:
        self.code = int(code)
        try:
            self.code_enum = ErrorCode(self.code)
        except ValueError:
            self.code_enum = ErrorCode.INTERNAL
        self.message = message or f"SWMM error {self.code}"
        super().__init__(self.message)

    def __repr__(self) -> str:
        return f"{type(self).__name__}(code={self.code}, message={self.message!r})"


# ---------------------------------------------------------------------------
# Subclasses — each also inherits from the closest stdlib exception so that
# callers can write ``except IndexError:`` / ``except ValueError:`` without
# importing engine-specific symbols.
# ---------------------------------------------------------------------------


class BadHandleError(EngineError, RuntimeError):
    """Engine handle is NULL or invalid (``SWMM_ERR_BADHANDLE``)."""


class BadIndexError(EngineError, IndexError):
    """Object index out of range (``SWMM_ERR_BADINDEX``).

    Also raised when a string ID is not found in a collection (the
    collection layer maps this to :class:`KeyError` first; see
    :func:`_check`).
    """


class BadParamError(EngineError, ValueError):
    """Invalid parameter value (``SWMM_ERR_BADPARAM``)."""


class LifecycleError(EngineError, RuntimeError):
    """Function called in the wrong engine lifecycle state (``SWMM_ERR_LIFECYCLE``)."""


class HotStartError(EngineError, RuntimeError):
    """Hot start file error (``SWMM_ERR_HOTSTART``)."""


class PluginError(EngineError, RuntimeError):
    """Plugin failure (``SWMM_ERR_PLUGIN``)."""


class FileError(EngineError, IOError):
    """File I/O failure — input, report, output, or generic I/O.

    Covers ``SWMM_ERR_INPFILE``, ``SWMM_ERR_RPTFILE``, ``SWMM_ERR_OUTFILE``,
    and ``SWMM_ERR_IO``.
    """


class ParseError(EngineError, ValueError):
    """Input file parse error (``SWMM_ERR_PARSE``)."""


class NumericalError(EngineError, RuntimeError):
    """Numerical failure such as divergence (``SWMM_ERR_NUMERICAL``)."""


class CRSError(EngineError, ValueError):
    """Coordinate reference system error (``SWMM_ERR_CRS``)."""


class DependencyError(EngineError, RuntimeError):
    """Object has dependents that block the requested operation
    (``SWMM_ERR_DEPENDENCY``)."""


class StaleObjectError(LifecycleError):
    """Wrapper object refers to a model state that has since changed.

    Raised by domain wrappers (:class:`Node`, :class:`Link`, …) when the
    collection's generation counter has advanced past the wrapper's. The
    canonical recovery is to re-look up the object by id from the
    current collection.

    Construction does not go through :func:`raise_for_code`; staleness is
    a Python-side condition, not a C return code.
    """

    def __init__(self, message: str = "stale wrapper; the model changed since it was looked up"):
        # Use INTERNAL as the sentinel code; this is not a C-side error.
        super().__init__(ErrorCode.INTERNAL.value, message)


# Map every known error code to the exception class that should be raised.
# Anything not listed here falls back to :class:`EngineError`.
_CODE_TO_EXCEPTION: dict[int, type[EngineError]] = {
    ErrorCode.NOMEM:       EngineError,            # MemoryError is the alternative; keep simple for now.
    ErrorCode.INPFILE:     FileError,
    ErrorCode.RPTFILE:     FileError,
    ErrorCode.OUTFILE:     FileError,
    ErrorCode.PARSE:       ParseError,
    ErrorCode.LIFECYCLE:   LifecycleError,
    ErrorCode.BADHANDLE:   BadHandleError,
    ErrorCode.BADINDEX:    BadIndexError,
    ErrorCode.BADPARAM:    BadParamError,
    ErrorCode.PLUGIN:      PluginError,
    ErrorCode.IO:          FileError,
    ErrorCode.HOTSTART:    HotStartError,
    ErrorCode.CRS:         CRSError,
    ErrorCode.NUMERICAL:   NumericalError,
    ErrorCode.DEPENDENCY:  DependencyError,
    ErrorCode.INTERNAL:    EngineError,
}


def raise_for_code(code: int, message: Optional[str] = None) -> None:
    """Raise the right :class:`EngineError` subclass for ``code``.

    Called from the shared C-level ``_check()`` helper in
    ``_common.pxd``. ``code == 0`` is a no-op.

    :param code: The C return code.
    :param message: Optional override; otherwise filled from the engine's
        own ``swmm_error_message`` lookup.
    """
    if code == 0:
        return
    try:
        enum_code = ErrorCode(code)
    except ValueError:
        enum_code = ErrorCode.INTERNAL
    cls = _CODE_TO_EXCEPTION.get(enum_code, EngineError)
    raise cls(code, message or "")
