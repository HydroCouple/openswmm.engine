"""Distinguish missing build features from broken binary dependencies."""
from pathlib import Path
import subprocess
import sys

import pytest
import openswmm


@pytest.mark.parametrize('module', ['_2d', '_geopackage'])
@pytest.mark.parametrize('missing', [True, False])
def test_optional_module_import_contract(module, missing):
    stage = str(Path(openswmm.__file__).parents[1])
    script = f'''
import sys, importlib.abc
sys.meta_path[:] = [f for f in sys.meta_path if type(f).__module__ != '_openswmm_editable']
sys.path.insert(0, {stage!r})
class Block(importlib.abc.MetaPathFinder):
    def find_spec(self, fullname, path=None, target=None):
        if fullname == 'openswmm.engine.{module}':
            if {missing!r}:
                raise ModuleNotFoundError('omitted build feature', name=fullname)
            raise ImportError('broken native dependency')
sys.meta_path.insert(0, Block())
import openswmm.engine as engine
assert not engine.{'HAS_2D' if module=='_2d' else 'HAS_GEOPACKAGE'}
namespace = {{}}
exec('from openswmm.engine import *', namespace)
'''
    result = subprocess.run([sys.executable, '-c', script], capture_output=True, text=True)
    if missing:
        assert result.returncode == 0, result.stderr
    else:
        assert result.returncode != 0
        assert 'broken native dependency' in result.stderr


def test_installed_typing_marker():
    assert Path(openswmm.__file__).with_name('py.typed').is_file()
