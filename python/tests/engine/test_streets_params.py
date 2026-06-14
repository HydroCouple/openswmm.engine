"""New-API gap closure — Streets.get_params (swmm_street_get_params).

Round-trips set_params -> get_params against the real engine (no mocks),
covering the newly-added inverse street-parameter accessor.
"""

from __future__ import annotations

import pytest

pytest.importorskip("openswmm.engine._infrastructure")


class TestStreetGetParams:
    def test_set_then_get_round_trip(self, opened_solver):
        streets = opened_solver.infrastructure.streets
        idx = streets.add("ST_TEST")
        streets.set_params(
            idx,
            t_crown=0.5, h_curb=0.6, sx=0.04, n_road=0.016,
            gutter_depres=2.0, gutter_width=2.0, sides=2,
            back_width=10.0, back_slope=0.02, back_n=0.02,
        )
        p = streets.get_params(idx)
        assert p["t_crown"] == pytest.approx(0.5)
        assert p["h_curb"] == pytest.approx(0.6)
        assert p["sx"] == pytest.approx(0.04)
        assert p["n_road"] == pytest.approx(0.016)
        assert p["sides"] == 2
        assert p["back_width"] == pytest.approx(10.0)
