"""P1.2 — ModelBuilder external-file slot accessors.

Round-trips ``set_file_path`` / ``get_file_path`` against the real engine
(no mocks), closing the last hard C-API↔Cython gap (``swmm_file_path_get`` /
``swmm_file_path_set`` + the ``SWMM_FilePathRole`` enum).
"""

from __future__ import annotations

import pytest

pytest.importorskip("openswmm.engine._model")

from openswmm.engine import FilePathRole, ModelBuilder


class TestScalarFilePathRoundTrip:
    def test_set_then_get_rainfall(self):
        m = ModelBuilder()
        m.set_file_path(FilePathRole.RAINFALL, "rainfall_data.dat")
        absolute, original = m.get_file_path(FilePathRole.RAINFALL)
        # The original token is preserved verbatim; the absolute path is the
        # engine's resolution (may be empty if never resolved on disk).
        assert original == "rainfall_data.dat"
        assert isinstance(absolute, str)

    def test_clear_with_empty_path(self):
        m = ModelBuilder()
        m.set_file_path(FilePathRole.RUNOFF, "runoff.dat")
        m.set_file_path(FilePathRole.RUNOFF, "")
        _absolute, original = m.get_file_path(FilePathRole.RUNOFF)
        assert original == ""


class TestFilePathRoleEnum:
    def test_role_values(self):
        # Values must mirror SWMM_FilePathRole exactly.
        assert FilePathRole.RAINFALL == 1
        assert FilePathRole.HOTSTART_SAVE == 8
        assert FilePathRole.TIMESERIES_DATA == 10
