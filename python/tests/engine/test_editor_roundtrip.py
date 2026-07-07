"""Round-trip coverage for the GUI-editor getters/setters newly exposed in the
Python bindings.

Mirrors ``test_streets_params.py``: each test drives a real engine (the
``opened_solver`` fixture, an OPENED/editable model) through set -> get and
asserts the getter is the exact inverse of the setter. These wrap the C API
additions verified bit-for-bit by ``tests/unit/engine/test_editor_roundtrip_api.cpp``.
"""

from __future__ import annotations

import pytest

pytest.importorskip("openswmm.engine._infrastructure")
pytest.importorskip("openswmm.engine._subcatchments")

from openswmm.engine import ConcentrationUnits, LidType  # noqa: E402


class TestPollutantUnits:
    def test_set_units_round_trip(self, opened_solver):
        pollutants = opened_solver.pollutants
        if len(pollutants) == 0:
            pytest.skip("model has no pollutants to re-unit")
        p = pollutants[0]
        original = p.units
        p.units = ConcentrationUnits.UG_PER_L
        assert int(p.units) == 1
        p.units = ConcentrationUnits.COUNT_PER_L
        assert int(p.units) == 2
        p.units = original
        assert p.units == original


class TestAquiferEvapPattern:
    def test_set_get_clear_round_trip(self, opened_solver):
        aq = opened_solver.aquifers
        aq.add("AQ_RT")
        assert aq.get_evap_pattern("AQ_RT") == ""
        aq.set_evap_pattern("AQ_RT", "ET_MONTHLY")
        assert aq.get_evap_pattern("AQ_RT") == "ET_MONTHLY"
        aq.set_evap_pattern("AQ_RT", None)
        assert aq.get_evap_pattern("AQ_RT") == ""


class TestSnowpackSurfaces:
    def test_surfaces_and_removal_round_trip(self, opened_solver):
        sp = opened_solver.snowpacks
        sp.add("SP_RT")

        sp.set_plowable("SP_RT", cmin=0.001, cmax=0.01, tbase=25.0,
                        fwfrac=0.10, sd0=1.0, fw0=0.5, last=0.2)
        g = sp.get_plowable("SP_RT")
        assert g["cmin"] == pytest.approx(0.001)
        assert g["tbase"] == pytest.approx(25.0)
        assert g["last"] == pytest.approx(0.2)

        sp.set_impervious("SP_RT", cmin=0.002, cmax=0.02, tbase=26.0,
                          fwfrac=0.11, sd0=1.1, fw0=0.6, last=3.0)
        assert sp.get_impervious("SP_RT")["last"] == pytest.approx(3.0)

        sp.set_pervious("SP_RT", cmin=0.003, cmax=0.03, tbase=27.0,
                        fwfrac=0.12, sd0=1.2, fw0=0.7, last=4.0)
        assert sp.get_pervious("SP_RT")["tbase"] == pytest.approx(27.0)

        sp.set_removal("SP_RT", dsnow=2.0, fout=0.1, fimp=0.2,
                       fperv=0.3, fimelt=0.4, fsubcatch=0.0)
        r = sp.get_removal("SP_RT")
        assert r["dsnow"] == pytest.approx(2.0)
        assert r["fperv"] == pytest.approx(0.3)

        sp.set_removal_subcatch("SP_RT", "S_DUMP")
        assert sp.get_removal_subcatch("SP_RT") == "S_DUMP"


class TestInletParams:
    def test_params_and_type_round_trip(self, opened_solver):
        inlets = opened_solver.infrastructure.inlets
        inlets.add("IN_RT", "GRATE")
        idx = len(inlets) - 1
        inlets.set_params(idx, length=2.0, width=1.5, grate_type="P-50",
                          open_area=0.8, splash_veloc=5.0)
        p = inlets.get_params(idx)
        assert p["length"] == pytest.approx(2.0)
        assert p["width"] == pytest.approx(1.5)
        assert p["grate_type"] == "P-50"
        assert p["open_area"] == pytest.approx(0.8)
        assert p["splash_veloc"] == pytest.approx(5.0)
        assert inlets.get_type(idx) == "GRATE"


class TestLidLayers:
    def test_layers_and_type_round_trip(self, opened_solver):
        lids = opened_solver.infrastructure.lids
        idx = lids.add("BC_RT", LidType.BIO_CELL)
        lids.set_surface(idx, storage=2.0, roughness=0.1, slope=1.0)
        lids.set_soil(idx, thick=12.0, porosity=0.5, fc=0.2, wp=0.1,
                      ksat=0.5, kslope=10.0)
        lids.set_storage(idx, thick=12.0, void_frac=0.75, ksat=0.5)
        lids.set_drain(idx, coeff=1.0, expon=0.5, offset=6.0)

        assert lids.get_surface(idx) == pytest.approx(
            {"storage": 2.0, "roughness": 0.1, "slope": 1.0})
        soil = lids.get_soil(idx)
        assert soil["fc"] == pytest.approx(0.2)
        assert soil["kslope"] == pytest.approx(10.0)
        assert lids.get_storage(idx)["void_frac"] == pytest.approx(0.75)
        assert lids.get_drain(idx)["offset"] == pytest.approx(6.0)
        assert lids.get_type(idx) == int(LidType.BIO_CELL)

    def test_pavement_and_drainmat_round_trip(self, opened_solver):
        lids = opened_solver.infrastructure.lids
        pp = lids.add("PP_RT", LidType.PERM_PAVEMENT)
        lids.set_pavement(pp, thick=6.0, void_ratio=0.15, frac_imperv=0.0,
                          ksat=100.0, clog_factor=0.0, regen_days=0.0)
        pav = lids.get_pavement(pp)
        assert pav["thick"] == pytest.approx(6.0)
        assert pav["void_ratio"] == pytest.approx(0.15)
        assert pav["ksat"] == pytest.approx(100.0)

        gr = lids.add("GR_RT", LidType.GREEN_ROOF)
        lids.set_drainmat(gr, thick=1.0, void_frac=0.5, roughness=0.1)
        assert lids.get_drainmat(gr) == pytest.approx(
            {"thick": 1.0, "void_frac": 0.5, "roughness": 0.1})
