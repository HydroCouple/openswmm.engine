"""Round-trips and documented refusals for ``solver.heat`` (``_heat.pyx``).

Covers:
  * ``heat.enabled`` — the ``[OPTIONS] HEAT_TRANSPORT`` toggle.
  * ``heat.modules`` — the ``[HEAT_FLUXES]`` module toggles as a mapping.
  * ``heat.radiative`` — the ``[RADIATIVE_FLUXES]`` scalar parameters.
  * ``heat.shortwave_mode`` / ``set_shortwave_timeseries`` /
    ``current_shortwave`` — the three mutually exclusive shortwave sources.
  * ``heat.solar`` / ``heat.solar_sited`` — ``[SOLAR_RADIATION]``.
  * ``heat.cloud`` — ``[CLOUD_COVER]`` plus ``configured`` / ``current`` /
    ``set_timeseries`` / ``clear``.
  * ``heat.sources`` — the ``[HEAT_SOURCES]`` GLOBAL inlet temperatures,
    ``is_configured``, ``clear`` and ``effective``.
  * ``heat.node_overrides`` — the NODE-scope override rows.
  * The documented refusals, each asserting *refused, not clamped*: the
    value is read back after the refusal and must be unchanged.

The model is authored inline because ``site_drainage_model.inp`` carries no
``[HEAT_*]`` sections at all. Both the ``.inp`` and its ``.heat`` component
config, and every report/output file, land in ``tests/engine/output`` so a
user can review them after a run.

@author: Caleb Buahin
"""

from __future__ import annotations

import os
import unittest

from openswmm.engine import (
    BadParamError,
    HeatCloudParam,
    HeatFluxModule,
    HeatNodeOverride,
    HeatRadiativeParam,
    HeatShortwaveMode,
    HeatSolarParam,
    HeatSourceKind,
)

from tests.engine._solver_cases import EngineSolverCase

_OUT_DIR = os.path.join(os.path.dirname(__file__), "output")

#: Component id the engine registers the heat coordinator under
#: (``kHeatId`` in ``src/engine/transport/components/HeatModule/
#: HeatComponent.cpp``).
_HEAT_COMPONENT = "org.hydrocouple.openswmm.heat"


def _heat_model(tag):
    """Write a heat-enabled ``.inp`` plus its ``.heat`` config; return the .inp.

    Section spellings are the engine's own: ``[HEAT_SOURCES]``,
    ``[HEAT_FLUXES]``, ``[RADIATIVE_FLUXES]``, ``[SOLAR_RADIATION]`` and
    ``[CLOUD_COVER]`` are parsed by ``HeatComponent.cpp``; the component is
    bound to the deck through ``[PROCESS_COMPONENTS] <id> config="…"``.

    ``[SOLAR_RADIATION]`` is deliberately **absent** so ``solar_sited`` is
    False on a freshly opened model — that is the precondition the
    ``HeatShortwaveMode.COMPUTED`` refusal test needs.
    """
    os.makedirs(_OUT_DIR, exist_ok=True)
    heat_name = f"heat_{tag}.heat"

    heat_cfg = (
        "[HEAT_SOURCES]\n"
        "DWF             GLOBAL  15.0\n"
        "INITIAL_STATE   GLOBAL  20.0\n"
        "DWF             NODE    J1  22.0\n"
        "\n[HEAT_FLUXES]\n"
        "RADIATIVE_EXCHANGE  ON\n"
        "\n[RADIATIVE_FLUXES]\n"
        "SHORTWAVE  GLOBAL  250.0\n"
        "\n[CLOUD_COVER]\n"
        "FRACTION  GLOBAL  0.25\n"
    )
    with open(os.path.join(_OUT_DIR, heat_name), "w") as f:
        f.write(heat_cfg)

    text = (
        "[TITLE]\n"
        "Python heat-binding round-trip deck\n"
        "\n[OPTIONS]\n"
        "FLOW_UNITS           CFS\n"
        "FLOW_ROUTING         DYNWAVE\n"
        "HEAT_TRANSPORT       ON\n"
        "START_DATE           06/21/2026\n"
        "START_TIME           12:00:00\n"
        "END_DATE             06/21/2026\n"
        "END_TIME             12:10:00\n"
        "ROUTING_STEP         10\n"
        "REPORT_STEP          00:05:00\n"
        "\n[TEMPERATURE]\n"
        "TIMESERIES  air_ts\n"
        "HUMIDITY    50.0\n"
        "\n[TIMESERIES]\n"
        "air_ts    06/21/2026 00:00 50.0\n"
        "air_ts    06/22/2026 00:00 50.0\n"
        "sw_ts     06/21/2026 00:00 700.0\n"
        "sw_ts     06/22/2026 00:00 700.0\n"
        "cloud_ts  06/21/2026 00:00 0.5\n"
        "cloud_ts  06/22/2026 00:00 0.5\n"
        "\n[JUNCTIONS]\n"
        ";;Name Elev MaxDepth InitDepth SurDepth Aponded\n"
        "J1  9.0  10  1.0  0  0\n"
        "J2  8.5  10  1.0  0  0\n"
        "\n[OUTFALLS]\n"
        "OUT  8.0  FREE  NO\n"
        "\n[CONDUITS]\n"
        "C1  J1  J2   500  0.013  0  0  0\n"
        "C2  J2  OUT  500  0.013  0  0  0\n"
        "\n[XSECTIONS]\n"
        "C1  CIRCULAR  3.0  0  0  0\n"
        "C2  CIRCULAR  3.0  0  0  0\n"
        "\n[PROCESS_COMPONENTS]\n"
        f'{_HEAT_COMPONENT} config="{heat_name}"\n'
        "\n[REPORT]\n"
        "INPUT NO\n"
    )
    path = os.path.join(_OUT_DIR, f"heat_{tag}.inp")
    with open(path, "w") as f:
        f.write(text)
    return path


class _HeatCase(EngineSolverCase):
    """``EngineSolverCase`` bound to the inline heat deck.

    Overriding :meth:`solver_files` is all it takes: every lifecycle factory
    (``opened_solver`` / ``initialized_solver`` / ``running_solver``) then
    builds a per-test deck and registers its own ``addCleanup``.
    """

    def solver_files(self):
        # Class AND method: several classes share a method name (e.g.
        # ``test_container_protocol``), and the method name alone would put
        # two tests on the same deck file.
        tag = "_".join(self.id().split(".")[-2:])
        inp = _heat_model(tag)
        base = os.path.splitext(inp)[0]
        return inp, base + ".rpt", base + ".out"


# ---------------------------------------------------------------------------
# Toggles and module mapping
# ---------------------------------------------------------------------------
class TestHeatToggles(_HeatCase):

    def test_enabled_reflects_option(self):
        s = self.opened_solver()
        self.assertTrue(s.heat.enabled)

    def test_modules_container_protocol(self):
        s = self.opened_solver()
        mods = s.heat.modules
        self.assertEqual(len(mods), len(HeatFluxModule))
        self.assertEqual(list(mods), list(HeatFluxModule))
        # The deck turned radiative exchange on and left the others off.
        self.assertTrue(mods[HeatFluxModule.RADIATIVE_EXCHANGE])
        self.assertFalse(mods[HeatFluxModule.SURFACE_EXCHANGE])
        self.assertIsInstance(repr(mods), str)

    def test_module_roundtrip_by_member_code_and_name(self):
        s = self.opened_solver()
        mods = s.heat.modules
        for key in (HeatFluxModule.SURFACE_EXCHANGE,
                    int(HeatFluxModule.LAYER_CONDUCTION),
                    "RADIATIVE_EXCHANGE"):
            with self.subTest(key=key):
                mods[key] = True
                self.assertTrue(mods[key])
                mods[key] = False
                self.assertFalse(mods[key])

    def test_unknown_module_key_raises_keyerror(self):
        s = self.opened_solver()
        with self.assertRaises(KeyError):
            s.heat.modules["NOT_A_MODULE"]
        with self.assertRaises(KeyError):
            s.heat.modules[99] = True


# ---------------------------------------------------------------------------
# [RADIATIVE_FLUXES]
# ---------------------------------------------------------------------------
class TestHeatRadiative(_HeatCase):

    def test_container_protocol(self):
        s = self.opened_solver()
        rad = s.heat.radiative
        self.assertEqual(len(rad), len(HeatRadiativeParam))
        self.assertEqual(list(rad), list(HeatRadiativeParam))
        self.assertIsInstance(repr(rad), str)

    def test_shortwave_roundtrip_in_constant_mode(self):
        s = self.opened_solver()
        self.assertEqual(s.heat.shortwave_mode, HeatShortwaveMode.CONSTANT)
        self.assertAlmostEqual(s.heat.radiative[HeatRadiativeParam.SHORTWAVE],
                               250.0)
        s.heat.radiative[HeatRadiativeParam.SHORTWAVE] = 312.5
        self.assertAlmostEqual(s.heat.radiative[HeatRadiativeParam.SHORTWAVE],
                               312.5)

    def test_fraction_params_roundtrip(self):
        s = self.opened_solver()
        rad = s.heat.radiative
        fractions = [p for p in HeatRadiativeParam
                     if p is not HeatRadiativeParam.SHORTWAVE]
        for i, param in enumerate(fractions):
            value = 0.10 + 0.05 * i
            with self.subTest(param=param):
                rad[param] = value
                self.assertAlmostEqual(rad[param], value)
                # The same key spelled as a name and as a code agree.
                self.assertAlmostEqual(rad[param.name], value)
                self.assertAlmostEqual(rad[int(param)], value)

    def test_fraction_out_of_range_is_refused_not_clamped(self):
        s = self.opened_solver()
        rad = s.heat.radiative
        rad[HeatRadiativeParam.ALBEDO] = 0.08
        for bad in (1.5, -0.01):
            with self.subTest(bad=bad):
                with self.assertRaises(BadParamError):
                    rad[HeatRadiativeParam.ALBEDO] = bad
                # Refused, not clamped: the previous value survives.
                self.assertAlmostEqual(rad[HeatRadiativeParam.ALBEDO], 0.08)

    def test_negative_shortwave_is_refused_not_clamped(self):
        s = self.opened_solver()
        rad = s.heat.radiative
        with self.assertRaises(BadParamError):
            rad[HeatRadiativeParam.SHORTWAVE] = -1.0
        self.assertAlmostEqual(rad[HeatRadiativeParam.SHORTWAVE], 250.0)

    def test_shortwave_write_refused_outside_constant_mode(self):
        """A constant is not read under TIMESERIES, so storing one is refused."""
        s = self.opened_solver()
        rad = s.heat.radiative
        before = rad[HeatRadiativeParam.SHORTWAVE]
        s.heat.set_shortwave_timeseries("sw_ts")
        self.assertEqual(s.heat.shortwave_mode, HeatShortwaveMode.TIMESERIES)
        with self.assertRaises(BadParamError):
            rad[HeatRadiativeParam.SHORTWAVE] = 999.0
        # Refused, not clamped, and not half-applied either.
        self.assertAlmostEqual(rad[HeatRadiativeParam.SHORTWAVE], before)
        # Switching the mode back makes the same write legal.
        s.heat.shortwave_mode = HeatShortwaveMode.CONSTANT
        rad[HeatRadiativeParam.SHORTWAVE] = 999.0
        self.assertAlmostEqual(rad[HeatRadiativeParam.SHORTWAVE], 999.0)


# ---------------------------------------------------------------------------
# Shortwave mode
# ---------------------------------------------------------------------------
class TestHeatShortwaveMode(_HeatCase):

    def test_timeseries_binding_switches_mode(self):
        s = self.opened_solver()
        s.heat.set_shortwave_timeseries("sw_ts")
        self.assertEqual(s.heat.shortwave_mode, HeatShortwaveMode.TIMESERIES)
        s.heat.shortwave_mode = HeatShortwaveMode.CONSTANT
        self.assertEqual(s.heat.shortwave_mode, HeatShortwaveMode.CONSTANT)
        # Round-trip through the name and the code spellings too.
        s.heat.shortwave_mode = "TIMESERIES"
        self.assertEqual(s.heat.shortwave_mode, HeatShortwaveMode.TIMESERIES)
        s.heat.shortwave_mode = int(HeatShortwaveMode.CONSTANT)
        self.assertEqual(s.heat.shortwave_mode, HeatShortwaveMode.CONSTANT)

    def test_unknown_timeseries_refused(self):
        s = self.opened_solver()
        with self.assertRaises(BadParamError):
            s.heat.set_shortwave_timeseries("no_such_series")
        self.assertEqual(s.heat.shortwave_mode, HeatShortwaveMode.CONSTANT)

    def test_computed_requires_latitude_and_longitude(self):
        """COMPUTED before the site is set is refused, and refused cleanly."""
        s = self.opened_solver()
        self.assertFalse(s.heat.solar_sited)
        with self.assertRaises(BadParamError):
            s.heat.shortwave_mode = HeatShortwaveMode.COMPUTED
        # Refused, not applied: the previous mode is still in effect.
        self.assertEqual(s.heat.shortwave_mode, HeatShortwaveMode.CONSTANT)

        # Latitude alone is still not a site.
        s.heat.solar[HeatSolarParam.LATITUDE] = 41.74
        self.assertFalse(s.heat.solar_sited)
        with self.assertRaises(BadParamError):
            s.heat.shortwave_mode = HeatShortwaveMode.COMPUTED
        self.assertEqual(s.heat.shortwave_mode, HeatShortwaveMode.CONSTANT)

        # Both coordinates: now the mode is reachable.
        s.heat.solar[HeatSolarParam.LONGITUDE] = -111.83
        self.assertTrue(s.heat.solar_sited)
        s.heat.shortwave_mode = HeatShortwaveMode.COMPUTED
        self.assertEqual(s.heat.shortwave_mode, HeatShortwaveMode.COMPUTED)

    def test_current_shortwave_is_readable_state(self):
        s = self.opened_solver()
        self.assertIsInstance(s.heat.current_shortwave, float)


# ---------------------------------------------------------------------------
# [SOLAR_RADIATION]
# ---------------------------------------------------------------------------
class TestHeatSolar(_HeatCase):

    def test_container_protocol(self):
        s = self.opened_solver()
        solar = s.heat.solar
        self.assertEqual(len(solar), len(HeatSolarParam))
        self.assertEqual(list(solar), list(HeatSolarParam))
        self.assertIsInstance(repr(solar), str)

    def test_roundtrip(self):
        s = self.opened_solver()
        solar = s.heat.solar
        expected = {
            HeatSolarParam.LATITUDE: 41.74,
            HeatSolarParam.LONGITUDE: -111.83,
            HeatSolarParam.TIMEZONE: -7.0,
            HeatSolarParam.ELEVATION: -430.0,   # below sea level is legal
            HeatSolarParam.TURBIDITY_380: 0.15,
            HeatSolarParam.TURBIDITY_500: 0.10,
            HeatSolarParam.PRECIP_WATER: 1.4,
            HeatSolarParam.OZONE: 0.3,
            HeatSolarParam.GROUND_ALBEDO: 0.2,
        }
        for param, value in expected.items():
            with self.subTest(param=param):
                solar[param] = value
                self.assertAlmostEqual(solar[param], value)

    def test_out_of_range_is_refused_not_clamped(self):
        s = self.opened_solver()
        solar = s.heat.solar
        solar[HeatSolarParam.LATITUDE] = 41.74
        with self.assertRaises(BadParamError):
            solar[HeatSolarParam.LATITUDE] = 120.0
        self.assertAlmostEqual(solar[HeatSolarParam.LATITUDE], 41.74)

    def test_ground_albedo_fraction_refused_not_clamped(self):
        s = self.opened_solver()
        solar = s.heat.solar
        solar[HeatSolarParam.GROUND_ALBEDO] = 0.2
        with self.assertRaises(BadParamError):
            solar[HeatSolarParam.GROUND_ALBEDO] = 1.4
        self.assertAlmostEqual(solar[HeatSolarParam.GROUND_ALBEDO], 0.2)


# ---------------------------------------------------------------------------
# [CLOUD_COVER]
# ---------------------------------------------------------------------------
class TestHeatCloud(_HeatCase):

    def test_container_protocol(self):
        s = self.opened_solver()
        cloud = s.heat.cloud
        self.assertEqual(len(cloud), len(HeatCloudParam))
        self.assertEqual(list(cloud), list(HeatCloudParam))
        self.assertIsInstance(repr(cloud), str)

    def test_roundtrip_and_configured(self):
        s = self.opened_solver()
        cloud = s.heat.cloud
        self.assertTrue(cloud.configured)
        self.assertAlmostEqual(cloud[HeatCloudParam.FRACTION], 0.25)
        for param, value in ((HeatCloudParam.FRACTION, 0.6),
                             (HeatCloudParam.SW_ATTEN_K, 0.75),
                             (HeatCloudParam.SW_ATTEN_N, 3.4),
                             (HeatCloudParam.LW_CLOUD_K, 0.30)):
            with self.subTest(param=param):
                cloud[param] = value
                self.assertAlmostEqual(cloud[param], value)
        self.assertTrue(cloud.configured)

    def test_current_is_readable_state(self):
        s = self.opened_solver()
        self.assertIsInstance(s.heat.cloud.current, float)

    def test_set_timeseries_marks_configured(self):
        s = self.opened_solver()
        s.heat.cloud.set_timeseries("cloud_ts")
        self.assertTrue(s.heat.cloud.configured)

    def test_unknown_timeseries_refused(self):
        s = self.opened_solver()
        with self.assertRaises(BadParamError):
            s.heat.cloud.set_timeseries("no_such_series")

    def test_clear_restores_clear_sky(self):
        s = self.opened_solver()
        self.assertTrue(s.heat.cloud.configured)
        s.heat.cloud.clear()
        self.assertFalse(s.heat.cloud.configured)

    def test_fraction_out_of_range_is_refused_not_clamped(self):
        """FRACTION is a fraction, not a percent — a 75 must not become 1.0."""
        s = self.opened_solver()
        cloud = s.heat.cloud
        with self.assertRaises(BadParamError):
            cloud[HeatCloudParam.FRACTION] = 75.0
        self.assertAlmostEqual(cloud[HeatCloudParam.FRACTION], 0.25)
        with self.assertRaises(BadParamError):
            cloud[HeatCloudParam.FRACTION] = -0.5
        self.assertAlmostEqual(cloud[HeatCloudParam.FRACTION], 0.25)

    def test_negative_coefficient_is_refused_not_clamped(self):
        s = self.opened_solver()
        cloud = s.heat.cloud
        cloud[HeatCloudParam.LW_CLOUD_K] = 0.30
        with self.assertRaises(BadParamError):
            cloud[HeatCloudParam.LW_CLOUD_K] = -0.1
        self.assertAlmostEqual(cloud[HeatCloudParam.LW_CLOUD_K], 0.30)


# ---------------------------------------------------------------------------
# [HEAT_SOURCES] — GLOBAL temperatures
# ---------------------------------------------------------------------------
class TestHeatSources(_HeatCase):

    def test_container_protocol(self):
        s = self.opened_solver()
        src = s.heat.sources
        self.assertEqual(len(src), len(HeatSourceKind))
        self.assertEqual(list(src), list(HeatSourceKind))
        self.assertIn(HeatSourceKind.DWF, src)
        self.assertIn("EXTERNAL_INFLOW", src)
        self.assertIn(0, src)
        self.assertNotIn("NOT_A_SOURCE", src)
        self.assertIsInstance(repr(src), str)

    def test_parsed_values_and_configured_flags(self):
        s = self.opened_solver()
        src = s.heat.sources
        self.assertAlmostEqual(src[HeatSourceKind.DWF], 15.0)
        self.assertAlmostEqual(src[HeatSourceKind.INITIAL_STATE], 20.0)
        self.assertTrue(src.is_configured(HeatSourceKind.DWF))
        # An unset source reads the 20 degC default but is NOT configured.
        self.assertFalse(src.is_configured(HeatSourceKind.RAINFALL))
        self.assertAlmostEqual(src[HeatSourceKind.RAINFALL], 20.0)

    def test_roundtrip_every_source(self):
        s = self.opened_solver()
        src = s.heat.sources
        for i, kind in enumerate(HeatSourceKind):
            value = -10.0 + 3.5 * i
            with self.subTest(kind=kind):
                src[kind] = value
                self.assertAlmostEqual(src[kind], value)
                self.assertAlmostEqual(src[kind.name], value)
                self.assertAlmostEqual(src[int(kind)], value)
                self.assertTrue(src.is_configured(kind))

    def test_clear_restores_default_and_keeps_node_overrides(self):
        s = self.opened_solver()
        src = s.heat.sources
        self.assertEqual(len(s.heat.node_overrides), 1)
        src.clear(HeatSourceKind.DWF)
        self.assertFalse(src.is_configured(HeatSourceKind.DWF))
        self.assertAlmostEqual(src[HeatSourceKind.DWF], 20.0)
        # NODE overrides are separate rows; clear() must not delete them.
        self.assertEqual(len(s.heat.node_overrides), 1)
        self.assertAlmostEqual(src.effective(HeatSourceKind.DWF, "J1"), 22.0)

    def test_effective_resolves_override_then_global(self):
        s = self.opened_solver()
        src = s.heat.sources
        self.assertAlmostEqual(src.effective(HeatSourceKind.DWF, "J1"), 22.0)
        self.assertAlmostEqual(src.effective(HeatSourceKind.DWF, "J2"), 15.0)
        # Node index and node id address the same node.
        j1 = s.nodes["J1"].index
        self.assertAlmostEqual(src.effective(HeatSourceKind.DWF, j1), 22.0)

    def test_out_of_range_temperature_is_refused_not_clamped(self):
        """The parser's [-50, 100] degC range, enforced identically here."""
        s = self.opened_solver()
        src = s.heat.sources
        for bad in (100.5, -50.5):
            with self.subTest(bad=bad):
                with self.assertRaises(BadParamError):
                    src[HeatSourceKind.DWF] = bad
                # Refused, not clamped: still the deck's 15 degC.
                self.assertAlmostEqual(src[HeatSourceKind.DWF], 15.0)

    def test_range_endpoints_are_accepted(self):
        s = self.opened_solver()
        src = s.heat.sources
        for good in (-50.0, 100.0):
            with self.subTest(good=good):
                src[HeatSourceKind.GW] = good
                self.assertAlmostEqual(src[HeatSourceKind.GW], good)

    def test_unknown_source_key_raises_keyerror(self):
        s = self.opened_solver()
        with self.assertRaises(KeyError):
            s.heat.sources["NOT_A_SOURCE"]


# ---------------------------------------------------------------------------
# [HEAT_SOURCES] — NODE overrides
# ---------------------------------------------------------------------------
class TestHeatNodeOverrides(_HeatCase):

    def test_container_protocol(self):
        s = self.opened_solver()
        rows = s.heat.node_overrides
        self.assertEqual(len(rows), 1)
        row = rows[0]
        self.assertIsInstance(row, HeatNodeOverride)
        self.assertEqual(row.source, HeatSourceKind.DWF)
        self.assertEqual(row.node_index, s.nodes["J1"].index)
        self.assertAlmostEqual(row.temp_c, 22.0)
        self.assertEqual(rows[-1], row)
        self.assertEqual(list(rows), [row])
        self.assertIsInstance(repr(rows), str)

    def test_index_out_of_range(self):
        s = self.opened_solver()
        with self.assertRaises(IndexError):
            s.heat.node_overrides[5]

    def test_set_updates_rather_than_duplicates(self):
        s = self.opened_solver()
        rows = s.heat.node_overrides
        rows.set(HeatSourceKind.DWF, "J1", 25.5)
        self.assertEqual(len(rows), 1)
        self.assertAlmostEqual(rows[0].temp_c, 25.5)

    def test_set_appends_a_new_pair_and_remove_shifts(self):
        s = self.opened_solver()
        rows = s.heat.node_overrides
        rows.set(HeatSourceKind.EXTERNAL_INFLOW, "J2", 12.0)
        self.assertEqual(len(rows), 2)
        added = rows[1]
        self.assertEqual(added.source, HeatSourceKind.EXTERNAL_INFLOW)
        self.assertEqual(added.node_index, s.nodes["J2"].index)
        self.assertAlmostEqual(added.temp_c, 12.0)
        rows.remove(0)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0], added)   # later rows shifted down

    def test_scope_rule_refuses_non_dwf_non_external(self):
        """Only DWF and EXTERNAL_INFLOW take NODE scope (the H1 rule)."""
        s = self.opened_solver()
        rows = s.heat.node_overrides
        before = len(rows)
        for kind in (HeatSourceKind.RAINFALL, HeatSourceKind.GW,
                     HeatSourceKind.RDII, HeatSourceKind.IFACE,
                     HeatSourceKind.INITIAL_STATE):
            with self.subTest(kind=kind):
                with self.assertRaises(BadParamError):
                    rows.set(kind, "J2", 18.0)
                # Refused, not deferred: no row was written.
                self.assertEqual(len(rows), before)

    def test_override_temperature_range_is_refused_not_clamped(self):
        s = self.opened_solver()
        rows = s.heat.node_overrides
        with self.assertRaises(BadParamError):
            rows.set(HeatSourceKind.DWF, "J1", 250.0)
        self.assertEqual(len(rows), 1)
        self.assertAlmostEqual(rows[0].temp_c, 22.0)

    def test_unknown_node_raises_keyerror(self):
        s = self.opened_solver()
        with self.assertRaises(KeyError):
            s.heat.node_overrides.set(HeatSourceKind.DWF, "NOPE", 20.0)


# ---------------------------------------------------------------------------
# Repr
# ---------------------------------------------------------------------------
class TestHeatRepr(_HeatCase):

    def test_repr_does_not_raise(self):
        s = self.opened_solver()
        for view in (s.heat, s.heat.modules, s.heat.radiative, s.heat.solar,
                     s.heat.cloud, s.heat.sources, s.heat.node_overrides):
            with self.subTest(view=type(view).__name__):
                self.assertIsInstance(repr(view), str)


if __name__ == "__main__":
    unittest.main()
