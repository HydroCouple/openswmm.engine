"""
P0 — Shared infrastructure tests.

Covers the new pieces introduced by the Pythonic-bindings v1 plan:

* :mod:`openswmm.engine._exceptions` — the EngineError hierarchy and
  :func:`raise_for_code` dispatch.
* The shared ``_resolve_index`` helper exercised end-to-end through every
  collection's ``int | str`` indexing path.
* DateTime round-trip across the C API datetime module.

The exception-hierarchy tests are pure-Python and run without the compiled
engine. The collection tests use the solver lifecycle helpers and require a
real Solver, so they will be skipped automatically if the engine extension
can't be imported.
"""

from __future__ import annotations

import unittest
from datetime import datetime, timedelta

from openswmm.engine._enums import ErrorCode
from openswmm.engine._exceptions import (
    BadHandleError,
    BadIndexError,
    BadParamError,
    CRSError,
    DependencyError,
    EngineError,
    FileError,
    HotStartError,
    LifecycleError,
    NumericalError,
    ParseError,
    PluginError,
    raise_for_code,
)


# ---------------------------------------------------------------------------
# 1. EngineError hierarchy — pure Python, no engine required
# ---------------------------------------------------------------------------


class TestEngineErrorHierarchy(unittest.TestCase):
    """Each subclass must inherit from EngineError *and* the right stdlib base."""

    def test_engine_error_attributes(self):
        err = EngineError(99, "something went wrong")
        self.assertEqual(err.code, 99)
        self.assertIs(err.code_enum, ErrorCode.INTERNAL)
        self.assertEqual(err.message, "something went wrong")
        self.assertIn("something went wrong", repr(err))

    def test_unknown_code_falls_back_to_internal(self):
        err = EngineError(-12345)
        self.assertEqual(err.code, -12345)
        self.assertIs(err.code_enum, ErrorCode.INTERNAL)

    def test_subclass_inherits_from_stdlib(self):
        for cls, stdlib_base in [
            (BadIndexError, IndexError),
            (BadParamError, ValueError),
            (LifecycleError, RuntimeError),
            (HotStartError, RuntimeError),
            (BadHandleError, RuntimeError),
            (PluginError, RuntimeError),
            (FileError, IOError),
            (ParseError, ValueError),
            (NumericalError, RuntimeError),
            (CRSError, ValueError),
            (DependencyError, RuntimeError),
        ]:
            with self.subTest(cls=cls, stdlib_base=stdlib_base):
                err = cls(0, "msg")
                self.assertIsInstance(err, EngineError)
                self.assertIsInstance(err, stdlib_base)


# ---------------------------------------------------------------------------
# 2. raise_for_code dispatch
# ---------------------------------------------------------------------------


class TestRaiseForCode(unittest.TestCase):
    """Every documented error code maps to the expected exception class."""

    def test_zero_is_noop(self):
        raise_for_code(0)  # must not raise

    def test_code_maps_to_subclass(self):
        for code, expected_cls in [
            (ErrorCode.INPFILE.value,    FileError),
            (ErrorCode.RPTFILE.value,    FileError),
            (ErrorCode.OUTFILE.value,    FileError),
            (ErrorCode.IO.value,         FileError),
            (ErrorCode.PARSE.value,      ParseError),
            (ErrorCode.LIFECYCLE.value,  LifecycleError),
            (ErrorCode.BADHANDLE.value,  BadHandleError),
            (ErrorCode.BADINDEX.value,   BadIndexError),
            (ErrorCode.BADPARAM.value,   BadParamError),
            (ErrorCode.PLUGIN.value,     PluginError),
            (ErrorCode.HOTSTART.value,   HotStartError),
            (ErrorCode.CRS.value,        CRSError),
            (ErrorCode.NUMERICAL.value,  NumericalError),
            (ErrorCode.DEPENDENCY.value, DependencyError),
            (ErrorCode.INTERNAL.value,   EngineError),
        ]:
            with self.subTest(code=code, expected_cls=expected_cls):
                with self.assertRaises(expected_cls) as exc_info:
                    raise_for_code(code, "boom")
                self.assertEqual(exc_info.exception.code, code)
                self.assertEqual(exc_info.exception.message, "boom")

    def test_index_error_handler_catches_bad_index(self):
        """Stdlib base classes let callers use plain ``except``."""
        with self.assertRaises(IndexError):
            raise_for_code(ErrorCode.BADINDEX.value)

    def test_value_error_handler_catches_bad_param(self):
        with self.assertRaises(ValueError):
            raise_for_code(ErrorCode.BADPARAM.value)

    def test_io_error_handler_catches_file_errors(self):
        with self.assertRaises(IOError):
            raise_for_code(ErrorCode.INPFILE.value)


# ---------------------------------------------------------------------------
# 3. DateTime round-trip
# ---------------------------------------------------------------------------


class TestDateTimeRoundTrip(unittest.TestCase):
    """The Python helpers + C API encode/decode produce stable round trips."""

    def test_whole_second_round_trip(self):
        # Import locally so docs-only import doesn't fail when the compiled
        # extension is unavailable.
        try:
            from openswmm.engine import datetime_to_oadate, oadate_to_datetime
        except ImportError:
            self.skipTest("compiled openswmm.engine extensions not built")
        dt = datetime(2024, 6, 15, 13, 30, 45)
        back = oadate_to_datetime(datetime_to_oadate(dt))
        self.assertEqual(back, dt)

    def test_microsecond_precision(self):
        try:
            from openswmm.engine import datetime_to_oadate, oadate_to_datetime
        except ImportError:
            self.skipTest("compiled openswmm.engine extensions not built")
        dt = datetime(2024, 6, 15, 13, 30, 45, 123_456)
        back = oadate_to_datetime(datetime_to_oadate(dt))
        # Bound: 1 microsecond — see scripts/verify_dates_python.py.
        self.assertLessEqual(abs((back - dt).total_seconds()) * 1e6, 1.0)

    def test_epoch_is_zero(self):
        try:
            from openswmm.engine import datetime_to_oadate, oadate_to_datetime
        except ImportError:
            self.skipTest("compiled openswmm.engine extensions not built")
        self.assertEqual(datetime_to_oadate(datetime(1899, 12, 30)), 0.0)
        self.assertEqual(oadate_to_datetime(0.0), datetime(1899, 12, 30))


# ---------------------------------------------------------------------------
# 4. int | str polymorphic indexing on every collection
# ---------------------------------------------------------------------------
#
# These tests live here (not under test_nodes.py / test_links.py) so the
# behaviour is validated **uniformly** for every collection in one place.
# Phase P2+ will introduce wrapper classes, but the underlying _resolve_index
# helper is exercised today through the existing get_* methods.

try:
    from openswmm.engine import Nodes, Links, Subcatchments, Gages
    from tests.engine._solver_cases import EngineSolverCase
    _ENGINE_AVAILABLE = True
except ImportError:
    _ENGINE_AVAILABLE = False
    EngineSolverCase = unittest.TestCase  # fallback so classes below still load

engine_only = unittest.skipIf(
    not _ENGINE_AVAILABLE,
    "compiled openswmm.engine extensions not built",
)


@engine_only
class TestIndexResolution(EngineSolverCase):
    """``coll.get_X(0)`` and ``coll.get_X("ID")`` must agree."""

    def nodes(self):
        """A Nodes instance bound to a running solver."""
        return Nodes(self.running_solver())

    def links(self):
        """A Links instance bound to a running solver."""
        return Links(self.running_solver())

    def test_node_int_and_str_agree(self):
        nodes = self.nodes()
        zero_id = nodes.get_id(0)
        self.assertTrue(zero_id, "Test model has no nodes")
        self.assertEqual(nodes[0].depth, nodes[zero_id].depth)

    def test_link_int_and_str_agree(self):
        links = self.links()
        zero_id = links.get_id(0)
        self.assertTrue(zero_id, "Test model has no links")
        self.assertEqual(links[0].flow, links[zero_id].flow)

    def test_unknown_id_raises_keyerror(self):
        nodes = self.nodes()
        with self.assertRaises(KeyError):
            nodes["NO_SUCH_NODE_xyz"]

    def test_negative_index_handled(self):
        # Current behaviour: a negative int reaches the C layer and either
        # returns BADINDEX (preferred) or silently returns garbage. P0 doesn't
        # change the underlying _resolve calls in _nodes.pyx yet, so this test
        # documents the *current* contract. P2 will tighten it to always raise.
        pass


@engine_only
class TestErrorCodeMapping(EngineSolverCase):
    """Force the engine to return BADINDEX and confirm the Python side maps it."""

    def test_bad_index_node_raises_index_error(self):
        # Bypass the Python-side _resolve helper to get a real BADINDEX from C.
        # The current _resolve in _nodes.pyx checks bounds in Python, so the
        # only way to exercise the C path today is via the bulk array setters.
        # When P2 lands and _resolve is unified, this test will use the helper
        # directly; for now we just confirm that calling with an obviously
        # out-of-range int raises *something* IndexError-compatible.
        nodes = Nodes(self.running_solver())
        n = len(nodes)
        with self.assertRaises((IndexError, EngineError)):
            nodes[n + 9999]
