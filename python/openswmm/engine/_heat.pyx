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
from typing import NamedTuple

from ._common cimport *
from ._enums import (
    HeatCloudParam, HeatFluxModule, HeatRadiativeParam, HeatShortwaveMode,
    HeatSolarParam, HeatSourceKind)


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

    ``SHORTWAVE`` is W/m2; every other parameter is a dimensionless fraction
    in ``[0, 1]``.
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
        :returns: ``SHORTWAVE`` in W/m2; all others a fraction in ``[0, 1]``.
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
        :param value: ``SHORTWAVE`` in W/m2 (must be >= 0); every other
            parameter a fraction in ``[0, 1]``.
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
# Heat view
# =============================================================================

class Heat:
    """``solver.heat`` — heat-transport configuration.

    Edits are live: setters mutate engine state and the flux modules re-read
    the configuration every step, so a mid-run edit takes effect on the next
    routing step. Out-of-range values are refused, never clamped, and a
    refused write leaves the previous value in place.
    """

    def __init__(self, solver):
        self._solver = solver
        self._modules = None
        self._radiative = None
        self._solar = None
        self._cloud = None
        self._sources = None
        self._node_overrides = None

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

    def __repr__(self) -> str:
        try:
            return (f"<Heat enabled={self.enabled} "
                    f"shortwave={self.shortwave_mode.name} "
                    f"overrides={len(self.node_overrides)}>")
        except Exception:
            return "<Heat (engine closed)>"
