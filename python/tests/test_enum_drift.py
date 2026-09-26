"""Native numeric selectors must remain lossless in the Python API."""
from pathlib import Path
import re
import pytest
# Load the enum module without importing optional compiled extensions.
import importlib.util
_spec = importlib.util.spec_from_file_location("_binding_enums", Path(__file__).resolve().parents[1] / "openswmm/engine/_enums.py")
_enums = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_enums)

MAPPING = {'RunoffTotal': 'SWMM_RUNOFF_', 'RoutingTotal': 'SWMM_ROUTING_', 'RefType': 'SWMM_REF_', 'FilePathRole': 'SWMM_FILE_', 'ForcingType': 'SWMM_FORCE_', 'EvapType': 'SWMM_EVAP_', 'TempSource': 'SWMM_TEMP_', 'WindType': 'SWMM_WIND_', 'HumidityType': 'SWMM_HUMIDITY_', 'HumidityVar': 'SWMM_HUMIDITY_', 'HeatElemKind': 'SWMM_HEAT_ELEM_', 'InpProfile': 'SWMM_INP_PROFILE_', 'TransportDispersionMode': 'SWMM_DISPERSION_', 'UnitSystem': 'SWMM_'}

@pytest.mark.parametrize("name,prefix", MAPPING.items())
def test_native_enum_members(name, prefix):
    headers = Path(__file__).resolve().parents[2] / "include/openswmm/engine"
    text = "\n".join(p.read_text() for p in headers.glob("*.h"))
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    match = re.search(r"typedef enum SWMM_" + name + r"\s*\{(.*?)\}", text, re.S)
    assert match, name
    values = re.findall(r"\b(" + prefix + r"\w+)\s*=\s*(-?\d+)", match[1])
    assert values, name
    enum = getattr(_enums, name)
    for symbol, value in values:
        member = symbol[len(prefix):]
        assert enum.__members__[member].value == int(value), symbol


def test_enum_stubs_match_runtime():
    import ast
    stub = Path(__file__).resolve().parents[1] / 'openswmm/engine/_enums.pyi'
    for node in ast.parse(stub.read_text()).body:
        if not isinstance(node, ast.ClassDef) or not hasattr(_enums, node.name):
            continue
        enum = getattr(_enums, node.name)
        members = {stmt.targets[0].id: ast.literal_eval(stmt.value)
                   for stmt in node.body if isinstance(stmt, ast.Assign)
                   and isinstance(stmt.targets[0], ast.Name)}
        assert members == {key: item.value for key, item in enum.__members__.items()}, node.name


MACROS = {
    'CellScope': ('SWMM_GW_SCOPE_', 'SWMM_GW2D_SCOPE_', 'SWMM_SQ2D_SCOPE_'),
    'GroundwaterSoil': ('SWMM_GW2D_SOIL_',),
    'GroundwaterClosure': ('SWMM_GW2D_CLOSURE_',),
    'GroundwaterVariable': ('SWMM_GW2D_VAR_',),
    'GroundwaterLedger': ('SWMM_GW2D_LED_',),
    'GroundwaterZone': ('SWMM_GW2D_ZONE_',),
    'GroundwaterSpeciesLedger': ('SWMM_GW2D_SPL_',),
    'GroundwaterTransportZone': ('SWMM_GW_ZONE_',),
    'TransportDomain': ('SWMM_TRANSPORT_DOMAIN_',),
    'TransportClass': ('SWMM_TRANSPORT_CLASS_',),
    'TransportState': ('SWMM_TRANSPORT_',),
}

@pytest.mark.parametrize('name,prefixes', MACROS.items())
def test_native_macro_selectors(name, prefixes):
    headers = Path(__file__).resolve().parents[2] / 'include/openswmm/engine'
    text = '\n'.join(p.read_text() for p in headers.glob('*.h'))
    enum = getattr(_enums, name)
    for prefix in prefixes:
        values = dict((symbol[len(prefix):], int(value)) for symbol, value in
                      re.findall(r'^#define\s+(' + prefix + r'\w+)\s+\(?(-?\d+)\)?', text, re.M))
        values.pop('COUNT', None)
        if name == 'TransportState':
            values = {key: value for key, value in values.items() if not key.startswith(('DOMAIN_', 'CLASS_'))}
        assert values, prefix
        assert values == {key: item.value for key, item in enum.__members__.items()}
