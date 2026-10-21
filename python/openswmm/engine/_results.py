# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Caleb Buahin
"""Discoverable, lazy result fields over the native 1D and 2D readers."""
from __future__ import annotations

from collections.abc import Iterator, Mapping
from contextlib import ExitStack
from dataclasses import dataclass
import operator
from os import PathLike
import re
from types import MappingProxyType
from typing import Any, Literal, cast

import numpy as np
from numpy.typing import NDArray

from ._enums import FlowUnits, OutNodeVar, OutLinkVar, OutSubcatchVar, OutSystemVar
from ._output_reader import OutputReader
from ._output2d import Output2DReader


def _indices(selection: Any, size: int, names: tuple[str, ...] = ()) -> tuple[list[int], bool]:
    """Resolve Python selectors without allocating an array the size of an axis."""
    if selection is None:
        selection = slice(None)
    if isinstance(selection, slice):
        return list(range(*selection.indices(size))), False
    if isinstance(selection, str) or np.isscalar(selection):
        if isinstance(selection, str):
            try:
                return [names.index(selection)], True
            except ValueError:
                raise KeyError(selection) from None
        index = operator.index(cast(Any, selection))
        index += size if index < 0 else 0
        if index < 0 or index >= size:
            raise IndexError('Result index out of range')
        return [index], True
    return [_indices(key, size, names)[0][0] for key in selection], False


def _selection_axes(selection: Any, rank: int) -> tuple[Any, ...]:
    keys = selection if isinstance(selection, tuple) else (selection,)
    ellipses = sum(key is Ellipsis for key in keys)
    if ellipses > 1:
        raise IndexError('Only one ellipsis is allowed')
    if ellipses:
        at = next(i for i, key in enumerate(keys) if key is Ellipsis)
        keys = keys[:at] + (slice(None),) * (rank - len(keys) + 1) + keys[at + 1:]
    if len(keys) > rank:
        raise IndexError('Too many selection axes')
    return keys + (slice(None),) * (rank - len(keys))


class ResultField:
    """A lazy numeric field. Inspect metadata, then call read() to load values.

    Fields retain their reader; arrays returned by read() remain valid after
    close. Missing optional conversion packages raise ImportError on use only.
    """
    name: str
    units: str | None
    dimensions: tuple[str, ...]

    @property
    def shape(self) -> tuple[int, ...]:
        raise NotImplementedError

    @property
    def times(self) -> NDArray[Any] | None:
        raise NotImplementedError

    def read(self, *, periods: Any = None, objects: Any = None,
             species: str | int | None = None, selection: Any = None) -> NDArray[Any]:
        raise NotImplementedError

    def __getitem__(self, selection: Any) -> NDArray[Any]:
        return self.read(selection=selection)

    def to_pandas(self):
        """Load as a Series or DataFrame; available for one or two dimensions."""
        import pandas as pd
        values = self.read()
        times = self.times
        if values.ndim == 1:
            result = pd.Series(values, index=times, name=self.name)
        elif values.ndim == 2:
            columns = getattr(self, 'object_ids', None)
            result = pd.DataFrame(values, index=times, columns=columns)
        else:
            raise ValueError('Use to_xarray() for fields with more than two dimensions')
        result.attrs['units'] = self.units
        return result

    def to_xarray(self):
        """Load as an xarray.DataArray, retaining named dimensions and units."""
        import xarray as xr
        coords = {}
        if self.times is not None:
            coords['time'] = self.times
        if hasattr(self, 'object_ids') and 'object' in self.dimensions:
            coords['object'] = list(self.object_ids)
        if hasattr(self, 'species_names') and 'species' in self.dimensions:
            coords['species'] = list(self.species_names)
        return xr.DataArray(self.read(), dims=self.dimensions, coords=coords,
                            name=self.name, attrs={'units': self.units})

    def __repr__(self) -> str:
        return f'{type(self).__name__}({self.name!r}, shape={self.shape}, units={self.units!r})'


def _units(reader: OutputReader, kind: str, name: str) -> str | None:
    si = int(reader.flow_units) >= int(FlowUnits.CMS)
    length, rain = ('m', 'mm') if si else ('ft', 'in')
    if name in {'evap', 'pet'}:
        return rain + '/day'
    if name in {'rainfall', 'infil'}:
        return rain + '/h'
    if name in {'snow_depth'}:
        return rain
    if name in {'depth', 'head', 'gw_elev'}:
        return length
    if name in {'volume', 'storage'}:
        return length + '3'
    if name == 'velocity':
        return length + '/s'
    if name in {'capacity', 'soil_moist', 'snow_albedo', 'snow_cover'}:
        return '1'
    if name in {'temperature'}:
        return 'degC' if si else 'degF'
    if name in {'snow_surface_temp', 'snow_bulk_temp'}:
        return 'degC'
    if name == 'snow_energy':
        return 'kJ/m2'
    if name == 'snow_swe':
        return 'm'
    if name in {'snow_melt', 'snow_input', 'snow_sublimation'}:
        return 'm/s'
    if name == 'snow_time':
        return 's'
    return {'CFS': 'ft3/s', 'GPM': 'gal/min', 'MGD': 'million gal/day',
            'CMS': 'm3/s', 'LPS': 'L/s', 'MLD': 'million L/day'}[reader.flow_units.name]


class SeriesField(ResultField):
    """One recorded 1D variable, optionally bound to one object."""
    def __init__(self, collection: ResultCollection, name: str, code: int,
                 units: str | None, object_index: int | None = None):
        self._collection = collection
        self.name, self._code, self.units = name, code, units
        self._object_index = object_index
        self.dimensions = ('time',) if object_index is not None or collection.kind == 'system' else ('time', 'object')

    @property
    def shape(self) -> tuple[int, ...]:
        size = self._collection.reader.period_count
        return (size,) if len(self.dimensions) == 1 else (size, len(self._collection))

    @property
    def times(self) -> NDArray[Any]:
        return self._collection.reader.period_times.copy()

    @property
    def object_ids(self) -> tuple[str, ...]:
        return self._collection.ids

    def read(self, *, periods: Any = None, objects: Any = None,
             species: str | int | None = None, selection: Any = None) -> NDArray[Any]:
        if species is not None:
            raise ValueError('Select a named species through the species mapping first')
        if selection is not None:
            if periods is not None or objects is not None:
                raise ValueError('Use selection or named selectors, not both')
            keys = _selection_axes(selection, len(self.dimensions))
            periods = keys[0] if keys else None
            objects = keys[1] if len(keys) > 1 else None
        collection = self._collection
        reader = collection.reader
        time_indices, scalar_time = _indices(periods, reader.period_count)
        scalar_field = len(self.dimensions) == 1
        if objects is not None and scalar_field:
            raise ValueError('This field is already bound to one object or the system')
        indices, scalar_object = ([self._object_index], True) if scalar_field else _indices(objects, len(collection), collection.ids)
        values = np.empty((len(time_indices), len(indices)), dtype=np.float32)
        if time_indices and indices:
            low, high = min(time_indices), max(time_indices)
            take = np.asarray(time_indices) - low
            # Bulk snapshots are efficient when reading every object. Small
            # selections use native object histories and never load other IDs.
            if not scalar_field and objects is None:
                method = getattr(reader, collection.kind + '_result')
                for row, period in enumerate(time_indices):
                    values[row] = method(period, self._code)
            else:
                method = getattr(reader, collection.kind + '_series')
                for col, index in enumerate(indices):
                    args = (self._code,) if collection.kind == 'system' else (index, self._code)
                    values[:, col] = method(*args, start=low, end=high)[take]
        if scalar_field or scalar_object:
            values = values[:, 0]
        if scalar_time:
            values = values[0]
        return np.asarray(values)


class _ResultObject:
    def __init__(self, collection: ResultCollection, index: int):
        self._collection, self._index = collection, index

    @property
    def id(self) -> str:
        return self._collection.ids[self._index]

    @property
    def variables(self) -> Mapping[str, SeriesField]:
        return MappingProxyType({name: self._bound(field) for name, field in self._collection.variables.items()})

    @property
    def species(self) -> Mapping[str, SeriesField]:
        return MappingProxyType({name: self._bound(field) for name, field in self._collection.species.items()})

    def _bound(self, field: SeriesField) -> SeriesField:
        return SeriesField(self._collection, field.name, field._code, field.units, self._index)

    def __getattr__(self, name: str) -> SeriesField:
        if name.startswith('_'):
            raise AttributeError(name)
        try:
            return self.variables[name]
        except KeyError:
            raise AttributeError(name) from None

    def __dir__(self) -> list[str]:
        return sorted(set(super().__dir__()) | set(self._collection.variables))


class ResultCollection(Mapping):
    """Recorded objects and named variables; indexing accepts an ID or index."""
    def __init__(self, reader: OutputReader, kind: str):
        self.reader, self.kind = reader, kind
        enum: Any = {'node': OutNodeVar, 'link': OutLinkVar,
                'subcatchment': OutSubcatchVar, 'system': OutSystemVar}[kind]
        self.ids = tuple(getattr(reader, kind + '_ids')) if kind != 'system' else ()
        fields = {}
        for member in enum:
            if member.name == 'POLLUT_BASE':
                continue
            if kind == 'subcatchment' and not reader.has_subcatch_variable(member.value):
                continue
            name = member.name.lower()
            fields[name] = SeriesField(self, name, member.value, _units(reader, kind, name))
        self.variables = MappingProxyType(fields)
        self.species = MappingProxyType({
            name: SeriesField(self, name, enum.POLLUT_BASE + i, units)
            for i, (name, units) in enumerate(zip(reader.pollutant_ids, reader.pollutant_units))
        }) if kind != 'system' else MappingProxyType({})

    def __len__(self) -> int:
        return len(self.ids)

    def __iter__(self) -> Iterator[str]:
        return iter(self.ids)

    def __getitem__(self, key: str | int) -> _ResultObject:
        indices, scalar = _indices(key, len(self.ids), self.ids)
        if not scalar:
            raise TypeError('Select one object by name or index')
        return _ResultObject(self, indices[0])

    def __getattr__(self, name: str) -> SeriesField:
        if name.startswith('_'):
            raise AttributeError(name)
        try:
            return self.variables[name]
        except KeyError:
            raise AttributeError(name) from None

    def __dir__(self) -> list[str]:
        return sorted(set(super().__dir__()) | set(self.variables))


@dataclass(frozen=True)
class ResultMesh:
    """Stored mesh in SI coordinates. Cells retain native order and padding -1."""
    vertices: NDArray[np.float64]
    cells: NDArray[np.int64]
    vertex_counts: NDArray[np.int64]
    crs: str | None
    metres_per_model_unit: float | None
    coordinate_units: str | None


def _axis_names(reader: Output2DReader, path: str, shape: tuple[int, ...],
                attrs: Mapping[str, str]) -> tuple[str, ...]:
    rank = len(shape)
    names = [f'axis_{i}' for i in range(rank)]
    layout = re.search(r'\[([^]]+)\]', attrs.get('layout', ''))
    if layout:
        tokens = [v.strip().lower() for v in layout.group(1).split(',')]
        if len(tokens) == rank:
            aliases = {'face': 'cell', 'nface': 'cell', 'ntime': 'time',
                       'nspecies': 'species', 'node': 'vertex', 'nnode': 'vertex'}
            names = [aliases.get(token, token) for token in tokens]
    timed = 0 in reader.unlimited_axes(path) and path.count('/') == 1
    if timed:
        names[0] = 'time'
    if attrs.get('location') == 'face' and 'cell' not in names and rank:
        names[-1] = 'cell'
    if path == '/Mesh2_edge_flux':
        names = ['time', 'cell', 'edge']
    elif path.startswith('/Mesh2_node_') and timed and rank == 2:
        names = ['time', 'vertex']
    elif path == '/Mesh2_face_nodes':
        names = ['cell', 'corner']
    elif rank == 1 and path.startswith('/Mesh2_face_'):
        names = ['cell']
    elif path.startswith('/Mesh2_edge_') and rank == 2:
        names = ['cell', 'edge']
    if rank >= 3 and 'species_names' in attrs and 'species' not in names:
        names[1] = 'species'
    # Unknown diagnostics keep positional axes rather than inventing a clock.
    return tuple(names)


class SurfaceField(ResultField):
    """A stored 2D dataset with named axes and optional bound cell/species."""
    def __init__(self, surface: SurfaceResults, path: str,
                 fixed: Mapping[str, int] | None = None):
        self._surface, self.path = surface, path
        self._fixed = dict(fixed or {})
        self.name = path.rsplit('/', 1)[-1].removeprefix('Mesh2_face_')
        self.attributes = MappingProxyType(surface.reader.attributes(path))
        self.units = self.attributes.get('units')
        shape, _ = surface.reader.shape(path)
        self._axes = _axis_names(surface.reader, path, shape, self.attributes)
        self.dimensions = tuple(axis for axis in self._axes if axis not in self._fixed)
        self.species_names = tuple(v.strip() for v in self.attributes.get('species_names', '').split(',') if v.strip())
        if 'species' in self._fixed:
            labels = self.attributes.get('species_units', '').split(',')
            index = self._fixed['species']
            self.units = labels[index].strip() if index < len(labels) and labels[index].strip() else None

    @property
    def shape(self) -> tuple[int, ...]:
        shape, _ = self._surface.reader.shape(self.path)
        return tuple(size for axis, size in zip(self._axes, shape) if axis not in self._fixed)

    @property
    def times(self) -> NDArray[Any] | None:
        if 'time' not in self.dimensions:
            return None
        times = self._surface.period_times
        if len(times) != self.shape[self.dimensions.index('time')]:
            raise ValueError(f'{self.path} has incomplete report rows; inspect it with selection= explicitly')
        return times

    def bind(self, **axes: int) -> SurfaceField:
        unknown = set(axes) - set(self._axes)
        if unknown:
            raise ValueError(f'Field has no axes: {sorted(unknown)}')
        return SurfaceField(self._surface, self.path, {**self._fixed, **axes})

    def read(self, *, periods: Any = None, objects: Any = None,
             species: str | int | None = None, selection: Any = None) -> NDArray[Any]:
        reader = self._surface.reader
        shape, dtype = reader.shape(self.path)
        keys: list[Any] = [self._fixed.get(axis, slice(None)) for axis in self._axes]
        if selection is not None:
            if periods is not None or objects is not None or species is not None:
                raise ValueError('Use selection or named selectors, not both')
            selected = _selection_axes(selection, len(self.dimensions))
            free_axes = [i for i, axis in enumerate(self._axes) if axis not in self._fixed]
            for i, key in zip(free_axes, selected):
                keys[i] = key
        for axis, key in (('time', periods), ('cell', objects), ('species', species)):
            if key is None:
                continue
            if axis not in self.dimensions:
                raise ValueError(f'Field has no unbound {axis} axis')
            if axis == 'species' and isinstance(key, str):
                try:
                    key = self.species_names.index(key)
                except ValueError:
                    raise KeyError(key) from None
            keys[self._axes.index(axis)] = key
        # Preserve ordered/duplicate cell lists using bounded per-cell slabs.
        if 'cell' in self._axes:
            cell_axis = self._axes.index('cell')
            key = keys[cell_axis]
            if not isinstance(key, slice) and not np.isscalar(key):
                indices, _ = _indices(key, shape[cell_axis])
                out_axis = sum(isinstance(k, slice) for k in keys[:cell_axis])
                if indices:
                    chunks = []
                    for index in indices:
                        slab = keys.copy()
                        slab[cell_axis] = index
                        chunks.append(reader.read(self.path, tuple(slab)))
                    return np.stack(chunks, axis=out_axis)
                keys[cell_axis] = slice(0, 0)
        return reader.read(self.path, tuple(keys))


class _SurfaceCell:
    def __init__(self, surface: SurfaceResults, index: int):
        self._surface, self.index = surface, index

    def __getattr__(self, name: str) -> SurfaceField:
        return getattr(self._surface, name).bind(cell=self.index)

    @property
    def species(self) -> Mapping[str, SurfaceField]:
        return MappingProxyType({name: field.bind(cell=self.index) for name, field in self._surface.species.items()})


class _SurfaceCells:
    def __init__(self, surface: SurfaceResults):
        self._surface = surface

    def __len__(self) -> int:
        return self._surface.reader.shape('/Mesh2_face_nodes')[0][0]

    def __getitem__(self, index: int) -> _SurfaceCell:
        indices, scalar = _indices(index, len(self))
        if not scalar:
            raise TypeError('Select one cell by its native zero-based index')
        return _SurfaceCell(self._surface, indices[0])


class SurfaceResults:
    """Saved 2D results. Each recorded dataset is accessible through field()."""
    def __init__(self, reader: Output2DReader):
        self.reader = reader
        self.cells = _SurfaceCells(self)
        self._paths = frozenset(reader.datasets)
        self._fields: dict[str, SurfaceField] = {}

    @property
    def variables(self) -> Mapping[str, SurfaceField]:
        return MappingProxyType({path[1:].removeprefix('Mesh2_face_'): self.field(path)
            for path in sorted(self._paths) if path.count('/') == 1 and path != '/time'})

    def field(self, path: str) -> SurfaceField:
        """Access a full dataset path, including nested diagnostic groups."""
        path = '/' + path.lstrip('/')
        if path not in self._paths:
            raise KeyError(f'Variable was not recorded: {path}')
        if path not in self._fields:
            self._fields[path] = SurfaceField(self, path)
        return self._fields[path]

    def __getattr__(self, name: str) -> SurfaceField:
        if name.startswith('_'):
            raise AttributeError(name)
        for path in ('/Mesh2_face_' + name, '/' + name):
            if path in self._paths:
                return self.field(path)
        raise AttributeError(f'No recorded 2D variable {name!r}; inspect variables')

    def __dir__(self) -> list[str]:
        names = {path[1:].removeprefix('Mesh2_face_') for path in self._paths if path.count('/') == 1}
        return sorted(set(super().__dir__()) | names)

    @property
    def period_times(self) -> NDArray[Any]:
        """Report dates from the file's declared CF units, at microsecond precision."""
        values = self.reader.read('/time')
        units = self.reader.attributes('/time').get('units', '')
        match = re.fullmatch(r'(days|hours|minutes|seconds) since (.+)', units)
        if match is None:
            raise ValueError(f'Unsupported or missing time units: {units!r}')
        scale = {'days': 86400, 'hours': 3600, 'minutes': 60, 'seconds': 1}[match[1]]
        if not np.isfinite(values).all():
            raise ValueError('Non-finite report dates in output')
        return np.datetime64(match[2], 'us') + np.rint(values * scale * 1e6).astype('timedelta64[us]')

    @property
    def mesh(self) -> ResultMesh:
        reader = self.reader
        vertices = np.column_stack([reader.read('/Mesh2_node_' + axis) for axis in 'xyz'])
        cells = reader.read('/Mesh2_face_nodes')
        attrs = reader.attributes('/Mesh2_face_nodes')
        fill = int(attrs.get('_FillValue', '-1'))
        valid = (cells != fill) & (cells >= 0)
        cells = np.where(valid, cells - int(attrs.get('start_index', '0')), -1)
        if '/Mesh2_face_nv' in self._paths:
            counts = reader.read('/Mesh2_face_nv')
            if np.any((counts < 3) | (counts > cells.shape[1])):
                raise ValueError('Invalid mesh cell vertex counts')
            cells[np.arange(cells.shape[1])[None, :] >= counts[:, None]] = -1
        else:
            counts = np.sum(cells >= 0, axis=1)
        if np.any(cells >= len(vertices)):
            raise ValueError('Mesh connectivity exceeds vertex count')
        try:
            crs = reader.attributes('/crs')
        except IndexError:
            crs = {}
        return ResultMesh(vertices, cells, counts,
            crs.get('model_crs'),
            float(crs['metres_per_model_unit']) if 'metres_per_model_unit' in crs else None,
            crs.get('units') or reader.attributes('/Mesh2_node_x').get('units'))

    @property
    def species(self) -> Mapping[str, SurfaceField]:
        path = '/Mesh2_face_species_conc'
        if path not in self._paths:
            return MappingProxyType({})
        field = self.field(path)
        return MappingProxyType({name: field.bind(species=i) for i, name in enumerate(field.species_names)})

    @property
    def mass_balance(self) -> Mapping[str, float]:
        prefix = '/mass_balance_2d/'
        result = {path[len(prefix):]: float(self.reader.read(path))
                  for path in sorted(self._paths) if path.startswith(prefix)}
        if result:
            attrs = self.reader.attributes('/mass_balance_2d')
            if 'continuity_error' in attrs:
                result['continuity_error'] = float(attrs['continuity_error'])
        return MappingProxyType(result)


class Results:
    """Own independent readers for a 1D output, a 2D output, or both.

    Open explicitly named paths; their reporting clocks remain independent.
    Use a context manager or close(). 2D files must be closed/immutable;
    live=True applies only to the binary 1D output.
    """
    def __init__(self, path: str | PathLike[str] | None = None, *,
                 surface: str | PathLike[str] | None = None, live: bool = False):
        if path is None and surface is None:
            raise ValueError('Provide a 1D output path, a 2D surface path, or both')
        self._stack = ExitStack()
        self.output: OutputReader | None = None
        self.surface: SurfaceResults | None = None
        try:
            if path is not None:
                self.output = self._stack.enter_context(OutputReader(path, live=live))
                self.nodes = ResultCollection(self.output, 'node')
                self.links = ResultCollection(self.output, 'link')
                self.subcatchments = ResultCollection(self.output, 'subcatchment')
                self.system = ResultCollection(self.output, 'system')
            if surface is not None:
                self.surface = SurfaceResults(self._stack.enter_context(Output2DReader(surface)))
        except BaseException:
            self.close()
            raise

    @classmethod
    def open(cls, path: str | PathLike[str] | None = None, *,
             surface: str | PathLike[str] | None = None, live: bool = False) -> Results:
        return cls(path, surface=surface, live=live)

    @property
    def times(self) -> NDArray[Any]:
        """1D report dates. The surface has its own period_times coordinate."""
        if self.output is None:
            raise ValueError('No 1D output was opened; use surface.period_times')
        return self.output.period_times.copy()

    def refresh(self) -> int:
        """Refresh complete 1D report periods; does not tail the 2D HDF5 file."""
        if self.output is None:
            raise ValueError('No 1D output was opened')
        return self.output.refresh()

    def close(self) -> None:
        self._stack.close()

    def __enter__(self) -> Results:
        if self.output is not None:
            _ = self.output.period_count
        if self.surface is not None:
            _ = self.surface.reader.datasets
        return self

    def __exit__(self, *args: Any) -> Literal[False]:
        self.close()
        return False
