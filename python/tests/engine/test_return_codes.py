"""C return codes say what went wrong (2026-10-24).

* ``SWMM_ERR_BUFSIZE`` (17) is what the text getters that refuse a short
  buffer return (``swmm_ueb_option_get``, ``swmm_climate_get_radiation_source``,
  ``swmm_climate_get_cloud``, ``swmm_snow2d_get_row``,
  ``swmm_gw2d_boundary_get``). ``SWMM_ERR_BADPARAM`` now means only a bad key,
  channel or argument, so the bindings grow the buffer on BUFSIZE alone and
  raise anything else at once.
* An out-of-range ``SAVE HOTSTART`` slot is ``SWMM_ERR_BADINDEX``.

Raw codes are read through ctypes, as ``test_results.py`` does. Outputs go to
``tests/_artifacts/`` for review.
"""

from __future__ import annotations

import ctypes as ct
import os
import unittest

try:
    import openswmm.engine as _engine
    import openswmm.engine._solver as _extension
    from openswmm.engine import BadParamError, ErrorCode, Solver
except ImportError as _exc:  # pragma: no cover - environment dependent
    raise unittest.SkipTest(f"requires compiled engine: {_exc}")

from tests._paths import SITE_DRAINAGE_INP, artifact_dir

_API = ct.CDLL(_extension.__file__)
_H = ct.c_void_p
LONG = "series_" + "é" * 1500  # 3,007 bytes: past every starting buffer


def _fn(name, *argtypes, restype=ct.c_int):
    fn = getattr(_API, name)
    fn.argtypes, fn.restype = list(argtypes), restype
    return fn


class _OpenedModel(unittest.TestCase):
    def setUp(self):
        out = artifact_dir(self)
        self.solver = Solver(SITE_DRAINAGE_INP, os.path.join(out, "model.rpt"),
                             os.path.join(out, "model.out"))
        self.solver.open()
        self.addCleanup(self.solver.close)
        self.h = _H(self.solver.handle)


class TestBufferTooSmall(_OpenedModel):
    def test_code_and_message(self):
        self.assertEqual(ErrorCode.BUFSIZE, 17)
        message = _fn("swmm_error_message", ct.c_int, restype=ct.c_char_p)
        self.assertIn(b"buffer too small", message(17).lower())

    def test_ueb_option_short_buffer_is_not_an_unknown_key(self):
        get = _fn("swmm_ueb_option_get", _H, ct.c_char_p, ct.c_char_p, ct.c_int)
        buf = ct.create_string_buffer(64)
        self.assertEqual(get(self.h, b"SURFACE_TEMP", buf, 64), ErrorCode.OK)
        value = buf.value
        short = ct.create_string_buffer(b"x" * (len(value) - 1), len(value))
        self.assertEqual(get(self.h, b"SURFACE_TEMP", short, len(value)), ErrorCode.BUFSIZE)
        self.assertEqual(short.value, b"x" * (len(value) - 1))  # nothing written
        self.assertEqual(get(self.h, b"NOT_A_KEY", buf, 64), ErrorCode.BADPARAM)
        self.assertEqual(self.solver.snow.option("SURFACE_TEMP"), value.decode())
        with self.assertRaises(BadParamError):
            self.solver.snow.option("NOT_A_KEY")

    def test_radiation_source(self):
        get = _fn("swmm_climate_get_radiation_source", _H, ct.c_int, ct.c_char_p, ct.c_int)
        self.solver.snow.set_radiation_source(0, LONG)
        buf = ct.create_string_buffer(256)
        self.assertEqual(get(self.h, 0, buf, 256), ErrorCode.BUFSIZE)
        self.assertEqual(get(self.h, 3, buf, 256), ErrorCode.BADPARAM)  # no such channel
        self.assertEqual(self.solver.snow.radiation_source(0), LONG)
        with self.assertRaises(BadParamError):
            self.solver.snow.radiation_source(3)

    @unittest.skipUnless(getattr(_engine, "HAS_2D", False), "needs the 2D module")
    def test_groundwater_boundary_series(self):
        get = _fn("swmm_gw2d_boundary_get", _H, ct.c_int, ct.c_void_p, ct.c_char_p, ct.c_int)
        gw = self.solver.groundwater2d
        gw.set_authored_boundary(0, 1, "HEAD", 2.5, LONG)
        row = ct.create_string_buffer(64)  # room for SWMM_GwBoundary
        buf = ct.create_string_buffer(256)
        self.assertEqual(get(self.h, 0, row, buf, 256), ErrorCode.BUFSIZE)
        self.assertEqual(get(self.h, 1, row, buf, 256), ErrorCode.BADINDEX)
        self.assertEqual(gw.authored_boundaries[0]["series"], LONG)

    @unittest.skipUnless(getattr(_engine, "HAS_2D", False), "needs the 2D module")
    def test_mesh_snow_row_index_and_section(self):
        get = _fn("swmm_snow2d_get_row", _H, ct.c_int, ct.c_int, ct.POINTER(ct.c_int),
                  ct.c_char_p, ct.c_int, ct.c_char_p, ct.c_int, ct.c_void_p)
        cell = ct.c_int()
        tag, pack = ct.create_string_buffer(64), ct.create_string_buffer(64)
        values = ct.create_string_buffer(64)  # room for SWMM_MeshSnowOverrides
        for row in (0, -1):  # the model has no mesh-snow rows
            self.assertEqual(get(self.h, 1, row, ct.byref(cell), tag, 64, pack, 64, values),
                             ErrorCode.BADINDEX)
        self.assertEqual(get(self.h, 3, 0, ct.byref(cell), tag, 64, pack, 64, values),
                         ErrorCode.BADPARAM)


class TestHotstartSlotIndex(_OpenedModel):
    def setUp(self):
        super().setUp()
        add = _fn("swmm_hotstart_saves_add", _H, ct.c_char_p, ct.c_double)
        self.assertEqual(add(self.h, b"slot.hsf", 0.0), ErrorCode.OK)

    def test_out_of_range_slot_is_an_index_error(self):
        buf = ct.create_string_buffer(64)
        out_d, out_i = ct.c_double(), ct.c_int()
        calls = {
            "get_path": (lambda i: _fn("swmm_hotstart_saves_get_path", _H, ct.c_int, ct.c_char_p,
                                       ct.c_int)(self.h, i, buf, 64)),
            "get_datetime": (lambda i: _fn("swmm_hotstart_saves_get_datetime", _H, ct.c_int,
                                           ct.POINTER(ct.c_double))(self.h, i, ct.byref(out_d))),
            "set_path": (lambda i: _fn("swmm_hotstart_saves_set_path", _H, ct.c_int,
                                       ct.c_char_p)(self.h, i, b"other.hsf")),
            "set_datetime": (lambda i: _fn("swmm_hotstart_saves_set_datetime", _H, ct.c_int,
                                           ct.c_double)(self.h, i, 1.0)),
            "get_format": (lambda i: _fn("swmm_hotstart_saves_get_format", _H, ct.c_int,
                                         ct.POINTER(ct.c_int))(self.h, i, ct.byref(out_i))),
            "set_format": (lambda i: _fn("swmm_hotstart_saves_set_format", _H, ct.c_int,
                                         ct.c_int)(self.h, i, 0)),
            "remove": (lambda i: _fn("swmm_hotstart_saves_remove", _H, ct.c_int)(self.h, i)),
        }
        for name, call in calls.items():
            for index in (1, -1):
                with self.subTest(function=name, index=index):
                    self.assertEqual(call(index), ErrorCode.BADINDEX)
            with self.subTest(function=name, index=0):
                self.assertEqual(call(0), ErrorCode.OK)

    def test_a_missing_buffer_is_still_a_bad_parameter(self):
        get = _fn("swmm_hotstart_saves_get_path", _H, ct.c_int, ct.c_char_p, ct.c_int)
        self.assertEqual(get(self.h, 0, None, 0), ErrorCode.BADPARAM)


if __name__ == "__main__":
    unittest.main()
