"""Runtime coverage for the public 2D soil-heat Python bindings."""

import numpy as np
import pytest

from openswmm.engine import (
    BadIndexError,
    ChannelGroundValue,
    GroundHeatLayerValue,
    GroundHeatValue,
    Solver,
)
from tests.engine._groundwater_cases import MODEL, cleanup


def _open_thermal_model(directory):
    path = directory / "ground_heat.inp"
    text = MODEL.replace("[OPTIONS]", "[OPTIONS]\nHEAT_TRANSPORT YES", 1)
    text += """
[2D_AQUIFER_OPTIONS]
NODE_ENROLMENT ROWS
GW_ET NONE
M_LAYERS 4
CLOSURE CLOSED_FORM
[2D_AQUIFER]
* 36 1 .45 .1 2 HG0 .5
[GW_TRANSPORT_OPTIONS]
THERMAL_MODEL POROUS_EQUILIBRIUM
CONDUCTION YES
LATERAL_CONDUCTION NO
SURFACE_THERMAL_BC ADIABATIC
DEEP_THERMAL_BC ADIABATIC
[GW_INITIAL_QUALITY]
* SAT __TEMPERATURE__ -3
* UNSAT __TEMPERATURE__ -3
"""
    path.write_text(text)
    solver = Solver(path, path.with_suffix(".rpt"), path.with_suffix(".out"))
    solver.open()
    solver.options.ext["HEAT_MODEL"] = "COUPLED"
    solver.options.ext["BED_HEAT_MODEL"] = "GROUND_COLUMN"
    solver.options.ext["HEAT_SURFACE_EXCHANGE"] = "NO"
    solver.options.ext["HEAT_RADIATIVE_EXCHANGE"] = "NO"
    return solver


def test_ground_heat_runtime_values_and_layer_ranges(tmp_path):
    solver = _open_thermal_model(tmp_path)
    try:
        solver.initialize()
        solver.start(False)
        solver.step()

        heat = solver.heat
        status = heat.ground_status()
        assert status["requested"]
        assert status["initialized"]
        assert status["aquifer_cells"] == 2  # triangle and quad in the shared fixture
        assert status["thermal_layers"] > 0
        assert status["conduction_active"]
        assert "active" in status["reason"].lower()

        energy = np.asarray(heat.ground_values(GroundHeatValue.ENERGY, 0, 2))
        capacity = np.asarray(heat.ground_values(GroundHeatValue.CAPACITY, 0, 2))
        temperature = np.asarray(heat.ground_values(GroundHeatValue.TEMPERATURE, 0, 2))
        assert energy.shape == capacity.shape == temperature.shape == (2,)
        assert np.isfinite(energy).all()
        assert (capacity > 0).all()
        assert temperature == pytest.approx([-3.0, -3.0], abs=1e-6)

        layer_count = status["thermal_layers"]
        tops = np.asarray(heat.ground_layers(0, GroundHeatLayerValue.TOP, 0, layer_count))
        bottoms = np.asarray(heat.ground_layers(0, GroundHeatLayerValue.BOTTOM, 0, layer_count))
        layer_capacity = np.asarray(heat.ground_layers(0, GroundHeatLayerValue.CAPACITY, 0, layer_count))
        layer_temperature = np.asarray(heat.ground_layers(0, GroundHeatLayerValue.TEMPERATURE, 0, layer_count))
        assert len(tops) == len(bottoms) == len(layer_capacity) == len(layer_temperature) == layer_count
        assert np.all(bottoms > tops)
        assert np.all(layer_capacity > 0)
        assert layer_temperature == pytest.approx(np.full(layer_count, -3.0), abs=1e-6)

        with pytest.raises(BadIndexError):
            heat.ground_layers(0, GroundHeatLayerValue.TEMPERATURE, layer_count, 1)
        with pytest.raises(ValueError):
            heat.ground_values(GroundHeatValue.ENERGY, 0, -1)

        channel_status = heat.channel_ground_status()
        assert not channel_status["requested"]
        assert ChannelGroundValue.BED_TEMPERATURE == 0
    finally:
        cleanup(solver)
