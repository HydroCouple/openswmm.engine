# SPDX-License-Identifier: Apache-2.0
#
# Copyright 2026 Caleb Buahin
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""
Heat transport (Pythonic v1 surface)
====================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: Apache-2.0

``solver.heat`` is the editable heat-transport configuration surface: the
``[HEAT_FLUXES]`` module toggles, the ``[RADIATIVE_FLUXES]``,
``[SOLAR_RADIATION]`` and ``[CLOUD_COVER]`` parameter blocks, and the
``[HEAT_SOURCES]`` inlet-temperature table (a global degC per water source
plus node-scope overrides).

**Edits are live.** Setters mutate engine state and the flux modules re-read
the configuration every step, so a mid-run edit takes effect on the next
routing step.

**Values are refused, not clamped.** Every range check mirrors the deck
parser exactly, so a value the ``.inp`` rejects is a value this API rejects,
and a refused write does not take effect at all — it raises
:class:`~openswmm.engine.EngineError` and leaves the previous value in place.

.. code-block:: python

    from openswmm.engine import (
        Solver, HeatFluxModule, HeatShortwaveMode, HeatSolarParam,
        HeatRadiativeParam, HeatSourceKind)

    with Solver("model.inp") as s:
        s.heat.modules[HeatFluxModule.RADIATIVE_EXCHANGE] = True
        s.heat.radiative[HeatRadiativeParam.ALBEDO] = 0.08

        # COMPUTED shortwave requires an explicit site: latitude AND longitude.
        s.heat.solar[HeatSolarParam.LATITUDE] = 41.74
        s.heat.solar[HeatSolarParam.LONGITUDE] = -111.83
        assert s.heat.solar_sited
        s.heat.shortwave_mode = HeatShortwaveMode.COMPUTED

        s.heat.sources[HeatSourceKind.DWF] = 18.5
        s.heat.node_overrides.set(HeatSourceKind.DWF, "J1", 22.0)
        print(s.heat.sources.effective(HeatSourceKind.DWF, "J1"))
"""

# cython: language_level=3

from collections.abc import Iterator
from array import array
from typing import NamedTuple

from ._common cimport *
from ._enums import (
    HeatAttr, HeatCloudParam, HeatFluxModule, HeatOverrideScope,
    HeatRadiativeParam, HeatSedimentParam, HeatShortwaveMode, HeatSolarParam,
    HeatSourceKind, EngineState, ErrorCode)
from ._exceptions import StaleObjectError


cdef inline SWMM_Engine _h(solver):
    return <SWMM_Engine><size_t>solver.handle


cdef inline int _resolve_node(solver, key) except -1:
    return _resolve_index(
        _h(solver), key, swmm_node_index, swmm_node_count, "Node")


def _member(enum_cls, key):
    """Coerce *key* (member, int code, or member name) to *enum_cls*.

    :param enum_cls: The target ``IntEnum`` class.
    :param key: An enum member, its integer code, or its member name.
    :returns: The matching enum member.
    :raises KeyError: *key* names no member; the message lists the valid ones.
    """
    if isinstance(key, enum_cls):
        return key
    try:
        if isinstance(key, str):
            return enum_cls[key.upper()]
        return enum_cls(key)
    except (KeyError, ValueError):
        pass
    valid = ", ".join(m.name for m in enum_cls)
    raise KeyError(
        f"{key!r} is not a valid {enum_cls.__name__}; valid members: {valid}")


class HeatNodeOverride(NamedTuple):
    """One ``[HEAT_SOURCES]`` NODE-scope override row.

    :ivar source: The :class:`HeatSourceKind` the row overrides. Only
        ``DWF`` and ``EXTERNAL_INFLOW`` can appear here (the H1 scope rule).
    :ivar node_index: Zero-based index of the node the override applies to.
    :ivar temp_c: Inlet temperature for that (source, node) pair, degC.
    """

    source: HeatSourceKind
    node_index: int
    temp_c: float


# =============================================================================
# [HEAT_FLUXES]
# =============================================================================

class _HeatModules:
    """``solver.heat.modules`` — mapping keyed by :class:`HeatFluxModule`.

    Each flux module of heat plan section 2 toggles independently::

        s.heat.modules[HeatFluxModule.SURFACE_EXCHANGE] = True
        if s.heat.modules[HeatFluxModule.RADIATIVE_EXCHANGE]:
            ...
    """

    def __init__(self, solver):
        self._solver = solver

    def __len__(self) -> int:
        return len(HeatFluxModule)

    def __iter__(self) -> Iterator[HeatFluxModule]:
        return iter(HeatFluxModule)

    def __getitem__(self, key) -> bool:
        """Is the ``[HEAT_FLUXES]`` module *key* on?

        :param key: A :class:`HeatFluxModule` member, code, or name.
        :returns: ``True`` when the module is enabled.
        :raises KeyError: *key* is not a valid :class:`HeatFluxModule`.
        :raises EngineError: The engine refused the read.
        """
        cdef int on = 0
        module = _member(HeatFluxModule, key)
        _check(swmm_heat_get_module(_h(self._solver), int(module), &on))
        return on != 0

    def __setitem__(self, key, value) -> None:
        """Turn the ``[HEAT_FLUXES]`` module *key* on or off.

        The edit is live: the module is re-read on the next routing step.

        :param key: A :class:`HeatFluxModule` member, code, or name.
        :param value: ``True`` to enable the module, ``False`` to disable it.
        :raises KeyError: *key* is not a valid :class:`HeatFluxModule`.
        :raises EngineError: The engine refused the write.
        """
        module = _member(HeatFluxModule, key)
        _check(swmm_heat_set_module(
            _h(self._solver), int(module), 1 if value else 0))

    def __repr__(self) -> str:
        try:
            on = [m.name for m in HeatFluxModule if self[m]]
            return f"<HeatModules on={on}>"
        except Exception:
            return "<HeatModules (engine closed)>"


# =============================================================================
# [RADIATIVE_FLUXES]
# =============================================================================

class _HeatRadiative:
    """``solver.heat.radiative`` — mapping keyed by :class:`HeatRadiativeParam`.

    ``SHORTWAVE`` is W/m2; ``LANDCOVER_TEMPERATURE`` is Celsius (NaN
    inherits air temperature); the remaining parameters are fractions in ``[0, 1]``.
    """

    def __init__(self, solver):
        self._solver = solver

    def __len__(self) -> int:
        return len(HeatRadiativeParam)

    def __iter__(self) -> Iterator[HeatRadiativeParam]:
        return iter(HeatRadiativeParam)

    def __getitem__(self, key) -> float:
        """Read one ``[RADIATIVE_FLUXES]`` parameter.

        :param key: A :class:`HeatRadiativeParam` member, code, or name.
        :returns: ``SHORTWAVE`` in W/m2; ``LANDCOVER_TEMPERATURE`` in Celsius
            (NaN inherits air); remaining parameters as fractions in ``[0, 1]``.
        :raises KeyError: *key* is not a valid :class:`HeatRadiativeParam`.
        :raises EngineError: The engine refused the read.
        """
        cdef double value = 0.0
        param = _member(HeatRadiativeParam, key)
        _check(swmm_heat_get_radiative(_h(self._solver), int(param), &value))
        return value

    def __setitem__(self, key, double value) -> None:
        """Write one ``[RADIATIVE_FLUXES]`` parameter.

        Values are **refused, not clamped** — the parser's own rule, so the
        API and the deck agree, and a refused write does not take effect.

        :param key: A :class:`HeatRadiativeParam` member, code, or name.
        :param value: ``SHORTWAVE`` in W/m2 (must be >= 0);
            ``LANDCOVER_TEMPERATURE`` in Celsius (NaN restores air-temperature
            inheritance); remaining parameters as fractions in ``[0, 1]``.
        :raises KeyError: *key* is not a valid :class:`HeatRadiativeParam`.
        :raises EngineError: A fraction is outside ``[0, 1]``; ``SHORTWAVE``
            is negative; or ``SHORTWAVE`` is written while
            :attr:`Heat.shortwave_mode` is not
            :attr:`HeatShortwaveMode.CONSTANT` — a constant is not read in the
            other two modes, and storing one there would look configured while
            changing nothing. Switch the mode first.
        """
        param = _member(HeatRadiativeParam, key)
        _check(swmm_heat_set_radiative(_h(self._solver), int(param), value))

    def __repr__(self) -> str:
        try:
            return f"<HeatRadiative n={len(self)}>"
        except Exception:
            return "<HeatRadiative (engine closed)>"


# =============================================================================
# [SOLAR_RADIATION]
# =============================================================================

class _HeatSolar:
    """``solver.heat.solar`` — mapping keyed by :class:`HeatSolarParam`.

    These parameters are consulted under
    :attr:`HeatShortwaveMode.COMPUTED` only.
    """

    def __init__(self, solver):
        self._solver = solver

    def __len__(self) -> int:
        return len(HeatSolarParam)

    def __iter__(self) -> Iterator[HeatSolarParam]:
        return iter(HeatSolarParam)

    def __getitem__(self, key) -> float:
        """Read one ``[SOLAR_RADIATION]`` parameter.

        :param key: A :class:`HeatSolarParam` member, code, or name.
        :returns: ``LATITUDE`` / ``LONGITUDE`` in degrees (+N / +E),
            ``TIMEZONE`` in hours from UTC (+E), ``ELEVATION`` in metres,
            ``PRECIP_WATER`` and ``OZONE`` in cm, the turbidities as Bird
            aerosol optical depths, ``GROUND_ALBEDO`` as a fraction.
        :raises KeyError: *key* is not a valid :class:`HeatSolarParam`.
        :raises EngineError: The engine refused the read.
        """
        cdef double value = 0.0
        param = _member(HeatSolarParam, key)
        _check(swmm_heat_get_solar(_h(self._solver), int(param), &value))
        return value

    def __setitem__(self, key, double value) -> None:
        """Write one ``[SOLAR_RADIATION]`` parameter.

        Writing ``LATITUDE`` or ``LONGITUDE`` also marks it *explicitly
        provided*, which is exactly what
        :attr:`HeatShortwaveMode.COMPUTED` checks for — see
        :attr:`Heat.solar_sited`.

        :param key: A :class:`HeatSolarParam` member, code, or name.
        :param value: ``LATITUDE`` degrees ``[-90, 90]``, ``LONGITUDE``
            degrees ``[-180, 180]``, ``TIMEZONE`` hours from UTC,
            ``ELEVATION`` metres ``[-500, 9000]`` (below sea level is legal;
            never written means the climate state's elevation is used),
            ``GROUND_ALBEDO`` the **land** albedo ``[0, 1]`` — not the water's
            :attr:`HeatRadiativeParam.ALBEDO`.
        :raises KeyError: *key* is not a valid :class:`HeatSolarParam`.
        :raises EngineError: *value* is out of range. Refused, not clamped.
        """
        param = _member(HeatSolarParam, key)
        _check(swmm_heat_set_solar(_h(self._solver), int(param), value))

    def clear(self) -> None:
        """Clear the shared site before initialization; COMPUTED mode refuses it."""
        _check(swmm_heat_clear_solar_site(_h(self._solver)))


    def __repr__(self) -> str:
        try:
            return f"<HeatSolar n={len(self)}>"
        except Exception:
            return "<HeatSolar (engine closed)>"


# =============================================================================
# [CLOUD_COVER]
# =============================================================================

class _HeatCloud:
    """``solver.heat.cloud`` — mapping keyed by :class:`HeatCloudParam`.

    Writing any parameter marks cloud cover as configured; :meth:`clear`
    removes the section entirely and restores clear sky.
    """

    @property
    def timeseries(self):
        """Bound timeseries name, or an empty string when none is bound."""
        cdef bytearray buffer = bytearray(256)
        while True:
            _check(swmm_heat_get_cloud_timeseries(_h(self._solver), buffer, len(buffer)))
            value = bytes(buffer).split(b'\0', 1)[0]
            if len(value) < len(buffer) - 1:
                return value.decode('utf-8')
            buffer = bytearray(len(buffer) * 2)

    def __init__(self, solver):
        self._solver = solver

    def __len__(self) -> int:
        return len(HeatCloudParam)

    def __iter__(self) -> Iterator[HeatCloudParam]:
        return iter(HeatCloudParam)

    def __getitem__(self, key) -> float:
        """Read one ``[CLOUD_COVER]`` parameter.

        :param key: A :class:`HeatCloudParam` member, code, or name.
        :returns: ``FRACTION`` as C in ``[0, 1]``; the others as the
            Kasten-Czeplak ``k`` / ``n`` and Bolz ``k_lw`` coefficients
            (dimensionless).
        :raises KeyError: *key* is not a valid :class:`HeatCloudParam`.
        :raises EngineError: The engine refused the read.
        """
        cdef double value = 0.0
        param = _member(HeatCloudParam, key)
        _check(swmm_heat_get_cloud(_h(self._solver), int(param), &value))
        return value

    def __setitem__(self, key, double value) -> None:
        """Write one ``[CLOUD_COVER]`` parameter, marking cloud cover configured.

        :param key: A :class:`HeatCloudParam` member, code, or name.
        :param value: ``FRACTION`` in ``[0, 1]`` — a fraction, **not** a
            percent; the coefficients must be non-negative.
        :raises KeyError: *key* is not a valid :class:`HeatCloudParam`.
        :raises EngineError: ``FRACTION`` is outside ``[0, 1]`` or a
            coefficient is negative. Refused, not clamped.
        """
        param = _member(HeatCloudParam, key)
        _check(swmm_heat_set_cloud(_h(self._solver), int(param), value))

    # ---- Cloud cover ------------------------------------------------

    @property
    def configured(self) -> bool:
        """Is a ``[CLOUD_COVER]`` section in effect?

        :returns: ``True`` when cloud cover is configured.
        :raises EngineError: The engine refused the read.
        """
        cdef int configured = 0
        _check(swmm_heat_get_cloud_configured(_h(self._solver), &configured))
        return configured != 0

    @property
    def current(self) -> float:
        """Cloud fraction in effect at the **current** step, ``[0, 1]``.

        Read-only: this is state, not configuration.

        :returns: The current-step cloud fraction.
        :raises EngineError: The engine refused the read.
        """
        cdef double fraction = 0.0
        _check(swmm_heat_get_current_cloud(_h(self._solver), &fraction))
        return fraction

    def set_timeseries(self, str name) -> None:
        """Bind a ``[TIMESERIES]`` by name as the cloud-fraction record.

        :param name: Id of an existing ``[TIMESERIES]``.
        :raises EngineError: No timeseries of that name exists.
        """
        cdef bytes b = name.encode('utf-8')
        _check(swmm_heat_set_cloud_timeseries(_h(self._solver), b))

    def clear(self) -> None:
        """Clear ``[CLOUD_COVER]`` entirely — back to clear sky.

        This restores the exact H3 longwave path, not an approximation of it:
        the cloud factor becomes a literal ``1.0`` that the atmospheric
        emissivity short-circuits.

        :raises EngineError: The engine refused the edit.
        """
        _check(swmm_heat_clear_cloud(_h(self._solver)))

    def __repr__(self) -> str:
        try:
            return f"<HeatCloud configured={self.configured}>"
        except Exception:
            return "<HeatCloud (engine closed)>"


# =============================================================================
# [HEAT_SOURCES]
# =============================================================================

class _HeatSources:
    """``solver.heat.sources`` — the GLOBAL inlet temperature per water source.

    A mapping keyed by :class:`HeatSourceKind` whose values are degC. The
    table is a fixed enum extent, not a parsed list, so it is readable even on
    a model with no heat configured; a source with no row reads the 20 degC
    default — use :meth:`is_configured` to tell a default from an explicit
    value.

    Every refusal mirrors the ``[HEAT_SOURCES]`` parser exactly: values are
    **refused, never clamped**.
    """

    def __init__(self, solver):
        self._solver = solver

    def __len__(self) -> int:
        """Number of sources the table carries (7).

        :returns: The source count. Never fails on a model with no heat
            configured.
        :raises EngineError: The engine refused the read.
        """
        cdef int count = 0
        _check(swmm_heat_source_count(_h(self._solver), &count))
        return count

    def __iter__(self) -> Iterator[HeatSourceKind]:
        cdef int n = len(self)
        for i in range(n):
            yield HeatSourceKind(i)

    def __getitem__(self, key) -> float:
        """Read one source's GLOBAL inlet temperature.

        :param key: A :class:`HeatSourceKind` member, code, or name.
        :returns: The global inlet temperature in degC. A source with no row
            reads the 20 degC default.
        :raises KeyError: *key* is not a valid :class:`HeatSourceKind`.
        :raises EngineError: The engine refused the read.
        """
        cdef double temp_c = 0.0
        source = _member(HeatSourceKind, key)
        _check(swmm_heat_get_source_temp(
            _h(self._solver), int(source), &temp_c))
        return temp_c

    def __setitem__(self, key, double temp_c) -> None:
        """Write one source's GLOBAL inlet temperature and mark it configured.

        :param key: A :class:`HeatSourceKind` member, code, or name.
        :param temp_c: Inlet temperature in degC, within ``[-50, 100]`` —
            the parser's own range.
        :raises KeyError: *key* is not a valid :class:`HeatSourceKind`.
        :raises EngineError: *temp_c* is outside ``[-50, 100]``. Refused, not
            clamped; a refused write does not take effect.
        """
        source = _member(HeatSourceKind, key)
        _check(swmm_heat_set_source_temp(
            _h(self._solver), int(source), temp_c))

    def __contains__(self, key) -> bool:
        """Is *key* a source this table carries?"""
        try:
            _member(HeatSourceKind, key)
            return True
        except (KeyError, TypeError):
            return False

    def is_configured(self, source) -> bool:
        """Did the model set *source* explicitly, or is it taking the default?

        An editor needs the distinction to avoid writing rows a user never
        asked for.

        :param source: A :class:`HeatSourceKind` member, code, or name.
        :returns: ``True`` when the model set this source explicitly.
        :raises KeyError: *source* is not a valid :class:`HeatSourceKind`.
        :raises EngineError: The engine refused the read.
        """
        cdef int configured = 0
        kind = _member(HeatSourceKind, source)
        _check(swmm_heat_get_source_configured(
            _h(self._solver), int(kind), &configured))
        return configured != 0

    def clear(self, source) -> None:
        """Return *source* to the 20 degC default and mark it unconfigured.

        The writer then emits no row for it. NODE overrides are **untouched**
        — they are separate rows, and removing them silently would delete
        model the caller did not name.

        :param source: A :class:`HeatSourceKind` member, code, or name.
        :raises KeyError: *source* is not a valid :class:`HeatSourceKind`.
        :raises EngineError: The engine refused the edit.
        """
        kind = _member(HeatSourceKind, source)
        _check(swmm_heat_clear_source_temp(_h(self._solver), int(kind)))

    def effective(self, source, node) -> float:
        """The temperature *source* water actually enters *node* at (degC).

        The NODE override when one exists, else the GLOBAL value. Exposed so a
        caller reads the same resolution the engine uses rather than
        re-deriving the precedence and drifting from it.

        :param source: A :class:`HeatSourceKind` member, code, or name.
        :param node: ``int`` node index or ``str`` node id.
        :returns: The effective inlet temperature in degC.
        :raises KeyError: *source* is not a valid :class:`HeatSourceKind`, or
            no node with that id exists.
        :raises EngineError: The engine refused the read.
        """
        cdef double temp_c = 0.0
        kind = _member(HeatSourceKind, source)
        cdef int ni = _resolve_node(self._solver, node)
        _check(swmm_heat_get_effective_source_temp(
            _h(self._solver), int(kind), ni, &temp_c))
        return temp_c

    def __repr__(self) -> str:
        try:
            configured = [s.name for s in self if self.is_configured(s)]
            return f"<HeatSources n={len(self)} configured={configured}>"
        except Exception:
            return "<HeatSources (engine closed)>"


class _HeatNodeOverrides:
    """``solver.heat.node_overrides`` — the NODE-scope override rows.

    A sequence of :class:`HeatNodeOverride` rows, indexed by row position.
    Only :attr:`HeatSourceKind.DWF` and
    :attr:`HeatSourceKind.EXTERNAL_INFLOW` may be overridden per node — the H1
    scope rule, refused rather than deferred silently, the same answer the
    deck gets.
    """

    def __init__(self, solver):
        self._solver = solver

    def __len__(self) -> int:
        """Number of NODE-scope override rows.

        :raises EngineError: The engine refused the read.
        """
        cdef int count = 0
        _check(swmm_heat_node_override_count(_h(self._solver), &count))
        return count

    def __iter__(self) -> Iterator[HeatNodeOverride]:
        cdef int n = len(self)
        for i in range(n):
            yield self[i]

    def __getitem__(self, int row_index) -> HeatNodeOverride:
        """Read one NODE override by row index.

        :param row_index: Zero-based row position; negative indexes from the
            end, as for a list.
        :returns: A :class:`HeatNodeOverride` of
            ``(source, node_index, temp_c)`` with *temp_c* in degC.
        :raises IndexError: *row_index* is out of range.
        :raises EngineError: The engine refused the read.
        """
        cdef int n = len(self)
        cdef int i = row_index + n if row_index < 0 else row_index
        if not (0 <= i < n):
            raise IndexError(row_index)
        cdef int source = 0
        cdef int node = -1
        cdef double temp_c = 0.0
        _check(swmm_heat_get_node_override(
            _h(self._solver), i, &source, &node, &temp_c))
        return HeatNodeOverride(HeatSourceKind(source), node, temp_c)

    def set(self, source, node, double temp_c) -> None:
        """Add or update the NODE override for ``(source, node)``.

        An existing ``(source, node)`` pair is **updated**, not duplicated:
        the deck parser refuses a duplicate row because a deck cannot mean two
        temperatures at once, while through an API setting the same pair twice
        is an edit. Same invariant — one row per pair — reached the way each
        caller means it.

        :param source: A :class:`HeatSourceKind` member, code, or name. Only
            :attr:`HeatSourceKind.DWF` and
            :attr:`HeatSourceKind.EXTERNAL_INFLOW` are in scope.
        :param node: ``int`` node index or ``str`` node id.
        :param temp_c: Inlet temperature in degC, within ``[-50, 100]``.
        :raises KeyError: *source* is not a valid :class:`HeatSourceKind`, or
            no node with that id exists.
        :raises EngineError: *source* is not ``DWF`` or ``EXTERNAL_INFLOW``
            (the H1 scope rule), *node* is out of range, or *temp_c* is
            outside ``[-50, 100]``. Refused, not clamped.
        """
        kind = _member(HeatSourceKind, source)
        cdef int ni = _resolve_node(self._solver, node)
        _check(swmm_heat_set_node_override(
            _h(self._solver), int(kind), ni, temp_c))
        self._solver._bump_generation()

    def remove(self, int row_index) -> None:
        """Remove one NODE override by row index.

        Later rows **shift down**, so a caller iterating by index must re-read
        the length after removing.

        :param row_index: Zero-based row position.
        :raises EngineError: The engine refused the removal.
        """
        _check(swmm_heat_remove_node_override(_h(self._solver), row_index))
        self._solver._bump_generation()

    def __repr__(self) -> str:
        try:
            return f"<HeatNodeOverrides n={len(self)}>"
        except Exception:
            return "<HeatNodeOverrides (engine closed)>"


# =============================================================================
# [SEDIMENT_EXCHANGE] and per-element overrides
# =============================================================================

class HeatElementOverride(NamedTuple):
    """One per-element heat attribute override row.

    :ivar attr: The :class:`HeatAttr` overridden.
    :ivar scope: :class:`HeatOverrideScope` — network ``TAG``/``NODE``/``LINK``
        or mesh ``CELL2D``/``MESH_TAG``. CELL2D names are one-based file IDs.
    :ivar name: Tag or element name.
    :ivar value: The override value, in the attribute's units.
    """

    attr: HeatAttr
    scope: HeatOverrideScope
    name: str
    value: float


class HeatLidMaterial(NamedTuple):
    """LID water/soil conductivity (W/m/K), soil density (kg/m3), heat capacity (J/kg/K)."""
    water_conductivity: float
    soil_conductivity: float
    soil_density: float
    soil_specific_heat: float


class HeatElementValue(NamedTuple):
    """Effective value and its origin; dynamic temperatures have a NaN value."""
    value: float
    source: int
    source_name: str


class _HeatSediment:
    """``solver.heat.sediment`` — ``[SEDIMENT_EXCHANGE]`` GLOBAL parameters,
    a mapping keyed by :class:`HeatSedimentParam`. Values the parser refuses
    are refused here too, never clamped."""

    def __init__(self, solver):
        self._solver = solver

    def __len__(self) -> int:
        return len(HeatSedimentParam)

    def __iter__(self) -> Iterator[HeatSedimentParam]:
        return iter(HeatSedimentParam)

    def __getitem__(self, key) -> float:
        cdef double value = 0.0
        param = _member(HeatSedimentParam, key)
        _check(swmm_heat_get_sediment(_h(self._solver), int(param), &value))
        return value

    def __setitem__(self, key, double value) -> None:
        param = _member(HeatSedimentParam, key)
        _check(swmm_heat_set_sediment(_h(self._solver), int(param), value))

    def is_configured(self, key) -> bool:
        """Authored status of GROUND_TEMPERATURE or INITIAL_TEMPERATURE."""
        cdef int configured = 0
        param = _member(HeatSedimentParam, key)
        _check(swmm_heat_sediment_temperature_configured(_h(self._solver), int(param), &configured))
        return bool(configured)

    def clear(self, key) -> None:
        """Restore temperature inheritance; ground reset also clears its series."""
        param = _member(HeatSedimentParam, key)
        _check(swmm_heat_clear_sediment_temperature(_h(self._solver), int(param)))

    @property
    def ground_timeseries(self) -> str:
        """Timeseries driving the ground temperature; ``''`` for the constant."""
        cdef char buf[1024]
        _check(swmm_heat_get_sediment_ground_timeseries(_h(self._solver), buf, 1024))
        return buf.decode('utf-8')

    @ground_timeseries.setter
    def ground_timeseries(self, name) -> None:
        cdef bytes b = (name or '').encode('utf-8')
        _check(swmm_heat_set_sediment_ground_timeseries(_h(self._solver), b))

    def __repr__(self) -> str:
        try:
            return f"<HeatSediment n={len(self)}>"
        except Exception:
            return "<HeatSediment (engine closed)>"


class _HeatElementOverrides:
    """``solver.heat.element_overrides`` — per-element radiative and sediment
    attribute overrides, a sequence of :class:`HeatElementOverride` rows."""

    def __init__(self, solver):
        self._solver = solver

    def __len__(self) -> int:
        cdef int count = 0
        _check(swmm_heat_element_override_count(_h(self._solver), &count))
        return count

    def __iter__(self) -> Iterator[HeatElementOverride]:
        cdef int n = len(self)
        for i in range(n):
            yield self[i]

    def __getitem__(self, int row_index) -> HeatElementOverride:
        cdef int n = len(self)
        cdef int i = row_index + n if row_index < 0 else row_index
        if not (0 <= i < n):
            raise IndexError(row_index)
        cdef int attr = 0, scope = 0
        cdef double value = 0.0
        cdef char name[512]
        _check(swmm_heat_get_element_override(
            _h(self._solver), i, &attr, &scope, name, 512, &value))
        return HeatElementOverride(HeatAttr(attr), HeatOverrideScope(scope),
                                   name.decode('utf-8'), value)

    def set(self, attr, scope, str name, double value) -> None:
        """Add or update the override for ``(attr, scope, name)``.

        Sediment attributes apply at LINK or TAG scope only (the bed lies
        beneath conduits). An unknown element or tag is refused and leaves
        the rows unchanged.
        """
        a = _member(HeatAttr, attr)
        sc = _member(HeatOverrideScope, scope)
        cdef bytes b = name.encode('utf-8')
        _check(swmm_heat_set_element_override(_h(self._solver), int(a), int(sc), b, value))
        self._solver._bump_generation()

    def remove(self, int row_index) -> None:
        """Remove one override row; later rows shift down."""
        _check(swmm_heat_remove_element_override(_h(self._solver), row_index))
        self._solver._bump_generation()

    def __repr__(self) -> str:
        try:
            return f"<HeatElementOverrides n={len(self)}>"
        except Exception:
            return "<HeatElementOverrides (engine closed)>"


cdef int _hydrothermal_edit(SWMM_Engine engine, void* user) noexcept with gil:
    context = <object>user
    try:
        context[3] = context[0](context[1])
        return 0
    except BaseException as error:
        context[2] = error
        return int(ErrorCode.BADPARAM)


cdef class HeatSnapshot:
    """Owned pre-run heat memento; lifecycle and structural edits invalidate it.

    Use as a context manager or call close(). Closing is safe after the solver
    is destroyed. Scalar heat edits may be undone repeatedly with restore().
    """
    cdef SWMM_HeatSnapshot _snapshot
    cdef object _owner
    cdef long long _generation

    def __init__(self, solver):
        if self._snapshot != NULL:
            raise RuntimeError("HeatSnapshot is already initialized")
        cdef SWMM_Engine handle = _h(solver)
        cdef int state = 0
        _check(swmm_engine_get_state(handle, &state))
        if state not in (int(EngineState.OPENED), int(EngineState.BUILDING)):
            _check(int(ErrorCode.LIFECYCLE))
        self._owner = solver
        self._generation = solver.generation
        self._snapshot = swmm_heat_snapshot_create(handle)
        if self._snapshot == NULL:
            raise MemoryError("Cannot capture heat configuration")

    def restore(self) -> None:
        """Restore to the original authoring session, refusing stale snapshots."""
        if self._snapshot == NULL:
            raise ValueError("HeatSnapshot is closed")
        if self._generation != self._owner.generation:
            raise StaleObjectError("HeatSnapshot was invalidated by a lifecycle or structural edit")
        cdef SWMM_Engine handle = _h(self._owner)
        _check(swmm_heat_snapshot_restore(handle, self._snapshot))
        self._owner._bump_generation()
        self._generation = self._owner.generation

    def close(self) -> None:
        """Release the memento without accessing the original engine."""
        if self._snapshot != NULL:
            swmm_heat_snapshot_destroy(self._snapshot)
            self._snapshot = NULL
        self._owner = None

    def __enter__(self):
        if self._snapshot == NULL:
            raise ValueError("HeatSnapshot is closed")
        return self

    def __exit__(self, *exc):
        self.close()

    def __dealloc__(self):
        if self._snapshot != NULL:
            swmm_heat_snapshot_destroy(self._snapshot)


# =============================================================================
# Heat view
# =============================================================================

class Heat:
    """``solver.heat`` — heat-transport configuration.

    Edits are live: setters mutate engine state and the flux modules re-read
    the configuration every step, so a mid-run edit takes effect on the next
    routing step. Out-of-range values are refused, never clamped, and a
    refused write leaves the previous value in place.
    """

    @property
    def shortwave_timeseries(self):
        """Bound timeseries name, or an empty string when none is bound."""
        cdef bytearray buffer = bytearray(256)
        while True:
            _check(swmm_heat_get_shortwave_timeseries(_h(self._solver), buffer, len(buffer)))
            value = bytes(buffer).split(b'\0', 1)[0]
            if len(value) < len(buffer) - 1:
                return value.decode('utf-8')
            buffer = bytearray(len(buffer) * 2)

    def __init__(self, solver):
        self._solver = solver
        self._modules = None
        self._radiative = None
        self._solar = None
        self._cloud = None
        self._sources = None
        self._node_overrides = None
        self._sediment = None
        self._element_overrides = None

    # ---- Toggles ----------------------------------------------------

    @property
    def enabled(self) -> bool:
        """Is ``[OPTIONS] HEAT_TRANSPORT`` on? Read-only.

        :returns: ``True`` when heat transport is enabled for the model.
        :raises EngineError: The engine refused the read.
        """
        cdef int enabled = 0
        _check(swmm_heat_get_enabled(_h(self._solver), &enabled))
        return enabled != 0

    @property
    def modules(self) -> _HeatModules:
        """``[HEAT_FLUXES]`` toggles: a mapping keyed by :class:`HeatFluxModule`
        whose values are ``bool``."""
        if self._modules is None:
            self._modules = _HeatModules(self._solver)
        return self._modules

    # ---- Radiative fluxes -------------------------------------------

    @property
    def radiative(self) -> _HeatRadiative:
        """``[RADIATIVE_FLUXES]``: a mapping keyed by
        :class:`HeatRadiativeParam` (``SHORTWAVE`` in W/m2, the rest
        fractions in ``[0, 1]``)."""
        if self._radiative is None:
            self._radiative = _HeatRadiative(self._solver)
        return self._radiative

    @property
    def shortwave_mode(self) -> HeatShortwaveMode:
        """Where incoming shortwave comes from — a :class:`HeatShortwaveMode`.

        The three modes are mutually exclusive **in effect**: exactly one is
        read, and there is no precedence ladder behind them. Switching modes
        does not erase the other modes' settings, so a GUI can offer three
        radio buttons without destroying what the user typed under the other
        two.

        :returns: The active :class:`HeatShortwaveMode`.
        :raises EngineError: The engine refused the read.
        """
        cdef int mode = 0
        _check(swmm_heat_get_shortwave_mode(_h(self._solver), &mode))
        return HeatShortwaveMode(mode)

    @shortwave_mode.setter
    def shortwave_mode(self, value) -> None:
        """Select the incoming-shortwave mode.

        :param value: A :class:`HeatShortwaveMode` member, code, or name.
        :raises KeyError: *value* is not a valid :class:`HeatShortwaveMode`.
        :raises EngineError: :attr:`HeatShortwaveMode.COMPUTED` was requested
            while latitude or longitude is unset (see :attr:`solar_sited` —
            the engine will **not** fall back on the ``[TEMPERATURE]``
            SNOWMELT latitude, which defaults to 0 and would silently model
            equatorial noon), or :attr:`HeatShortwaveMode.TIMESERIES` was
            requested with no series bound by
            :meth:`set_shortwave_timeseries`.
        """
        mode = _member(HeatShortwaveMode, value)
        _check(swmm_heat_set_shortwave_mode(_h(self._solver), int(mode)))

    def set_shortwave_timeseries(self, str name) -> None:
        """Bind a ``[TIMESERIES]`` by name as the shortwave record.

        This also switches :attr:`shortwave_mode` to
        :attr:`HeatShortwaveMode.TIMESERIES`.

        :param name: Id of an existing ``[TIMESERIES]`` (W/m2).
        :raises EngineError: No timeseries of that name exists.
        """
        cdef bytes b = name.encode('utf-8')
        _check(swmm_heat_set_shortwave_timeseries(_h(self._solver), b))

    @property
    def current_shortwave(self) -> float:
        """Resolved incoming shortwave at the **current** step, W/m2, cloud
        already applied.

        Read-only: this is state, not configuration. It is 0 before the first
        step, and whenever radiative exchange is off.

        :returns: Incoming shortwave in W/m2.
        :raises EngineError: The engine refused the read.
        """
        cdef double wm2 = 0.0
        _check(swmm_heat_get_current_shortwave(_h(self._solver), &wm2))
        return wm2

    # ---- Solar radiation --------------------------------------------

    @property
    def solar(self) -> _HeatSolar:
        """``[SOLAR_RADIATION]``: a mapping keyed by :class:`HeatSolarParam`,
        consulted under :attr:`HeatShortwaveMode.COMPUTED` only."""
        if self._solar is None:
            self._solar = _HeatSolar(self._solver)
        return self._solar

    @property
    def solar_sited(self) -> bool:
        """Have latitude **and** longitude both been explicitly set? Read-only.

        This is the precondition for :attr:`HeatShortwaveMode.COMPUTED`. A GUI
        should gate the COMPUTED radio button on this rather than discovering
        the refusal after the fact.

        :returns: ``True`` when both coordinates were written explicitly.
        :raises EngineError: The engine refused the read.
        """
        cdef int sited = 0
        _check(swmm_heat_get_solar_sited(_h(self._solver), &sited))
        return sited != 0

    # ---- Cloud cover ------------------------------------------------

    @property
    def cloud(self) -> _HeatCloud:
        """``[CLOUD_COVER]``: a mapping keyed by :class:`HeatCloudParam`, plus
        :attr:`~_HeatCloud.configured`, :attr:`~_HeatCloud.current`,
        :meth:`~_HeatCloud.set_timeseries` and :meth:`~_HeatCloud.clear`."""
        if self._cloud is None:
            self._cloud = _HeatCloud(self._solver)
        return self._cloud

    # ---- Heat sources -----------------------------------------------

    @property
    def sources(self) -> _HeatSources:
        """``[HEAT_SOURCES]`` GLOBAL inlet temperatures (degC), keyed by
        :class:`HeatSourceKind`."""
        if self._sources is None:
            self._sources = _HeatSources(self._solver)
        return self._sources

    @property
    def node_overrides(self) -> _HeatNodeOverrides:
        """``[HEAT_SOURCES]`` NODE-scope override rows, indexed by row
        position. Only ``DWF`` and ``EXTERNAL_INFLOW`` are in scope."""
        if self._node_overrides is None:
            self._node_overrides = _HeatNodeOverrides(self._solver)
        return self._node_overrides

    @property
    def sediment(self) -> _HeatSediment:
        """``[SEDIMENT_EXCHANGE]`` GLOBAL parameters, keyed by
        :class:`HeatSedimentParam`."""
        if self._sediment is None:
            self._sediment = _HeatSediment(self._solver)
        return self._sediment

    @property
    def element_overrides(self) -> _HeatElementOverrides:
        """Per-element radiative and sediment attribute overrides."""
        if self._element_overrides is None:
            self._element_overrides = _HeatElementOverrides(self._solver)
        return self._element_overrides

    @property
    def dry_policy(self) -> int:
        """Dry-element temperature policy: HOLD=0, AIR=1, DEFAULT=2."""
        cdef int policy = 0
        _check(swmm_heat_get_dry_policy(_h(self._solver), &policy))
        return policy

    @dry_policy.setter
    def dry_policy(self, int policy) -> None:
        _check(swmm_heat_set_dry_policy(_h(self._solver), policy))

    @property
    def lid_material(self) -> HeatLidMaterial:
        """Read-only LID conduction material assumptions, all in SI units."""
        cdef double values[4]
        _check(swmm_heat_get_lid_material(_h(self._solver), values))
        return HeatLidMaterial(values[0], values[1], values[2], values[3])

    @property
    def authoring_flags(self) -> int:
        """Provenance bits: configured=1, shared site=2, explicit lat/lon/timezone/elevation=4/8/16/32."""
        cdef unsigned flags = 0
        _check(swmm_heat_get_authoring_flags(_h(self._solver), &flags))
        return flags

    def effective_element(self, attr, scope, str name) -> HeatElementValue:
        """Read NODE/LINK/CELL2D precedence, independent of process activation.

        Source codes are GLOBAL=0, TAG=1, NODE=2, LINK=3, air=4, series=5,
        CELL2D=6, MESH_TAG=7 (mesh tags are independent of network tags).
        Dynamic temperatures return NaN and identify their source by name.
        """
        a = _member(HeatAttr, attr)
        sc = _member(HeatOverrideScope, scope)
        cdef bytes encoded = name.encode('utf-8')
        if b'\0' in encoded:
            raise ValueError("Element name contains a null byte")
        cdef double value = 0.0
        cdef int source = 0
        cdef bytearray buffer = bytearray(256)
        while True:
            _check(swmm_heat_get_effective_element(_h(self._solver), int(a), int(sc), encoded,
                                                   &value, &source, buffer, len(buffer)))
            origin = bytes(buffer).split(b'\0', 1)[0]
            if len(origin) < len(buffer) - 1:
                return HeatElementValue(value, source, origin.decode('utf-8'))
            buffer = bytearray(len(buffer) * 2)

    @property
    def mesh_status(self) -> dict:
        """Requested and initialized mesh heat, active modules and capability reason.

        Water heat capacity is J/(m3 K). Experimental status includes any
        qualification limits; saved switches alone do not imply activation.
        """
        cdef SWMM_Heat2DStatus status
        status.struct_size=sizeof(SWMM_Heat2DStatus)
        _check(swmm_heat_get_2d_status(_h(self._solver),&status))
        return dict(requested=bool(status.requested), initialized=bool(status.initialized),
                    surface_active=bool(status.surface_active),radiative_active=bool(status.radiative_active),
                    bed_active=bool(status.bed_active),bed_model=status.bed_model,
                    experimental=bool(status.experimental),water_heat_capacity=status.water_heat_capacity,
                    reason=(<bytes>status.reason).decode('utf-8'))

    def mesh_values(self, int kind, int first_cell, int count) -> list[float]:
        """Read a contiguous range of zero-based mesh cells, in SI units.

        Kind matches SWMM_Heat2DValue: water/bed temperature (0/1, degC),
        bed energy/capacity (2/3, J and J/K), atmospheric/radiation/sensible/
        latent/deep/water-to-bed/water increment (4..10, J), accepted
        evaporation (11, m3), thermal residual (12, J). Inactive results
        raise an engine lifecycle error rather than inventing values.
        """
        if count<0:
            raise ValueError("count must be nonnegative")
        cdef double[::1] values=array('d',[0.0])*count
        _check(swmm_heat_get_2d_values(_h(self._solver),kind,first_cell,count,
                                     &values[0] if count else NULL))
        return list(values)

    def snapshot(self) -> HeatSnapshot:
        """Capture heat settings, shared site and component registrations before initialization."""
        return HeatSnapshot(self._solver)

    def edit_atomic(self, callback):
        """Apply callback(solver) before initialization, rolling back on any exception.

        Only climate, snow, heat configuration, HEAT_TRANSPORT, component
        registrations and climate-file paths may be changed. Topology, lifecycle
        and nested transactions are forbidden. The callback result is returned;
        its original exception is re-raised after native rollback completes.
        """
        if not callable(callback):
            raise TypeError("callback must be callable")
        cdef SWMM_Engine handle = _h(self._solver)
        cdef int result
        context = [callback, self._solver, None, None]
        with self._solver._operation(<size_t>handle):
            result = swmm_hydrothermal_edit_atomic(handle, _hydrothermal_edit, <void*>context)
        self._solver._bump_generation()
        if context[2] is not None:
            raise context[2]
        _check(result)
        return context[3]

    def __repr__(self) -> str:
        try:
            return (f"<Heat enabled={self.enabled} "
                    f"shortwave={self.shortwave_mode.name} "
                    f"overrides={len(self.node_overrides)}>")
        except Exception:
            return "<Heat (engine closed)>"


cdef extern from "openswmm/engine/openswmm_heat.h":
    int swmm_heat_get_shortwave_timeseries(SWMM_Engine, char*, int)
    int swmm_heat_get_cloud_timeseries(SWMM_Engine, char*, int)
