"""Ensure new documented view signatures match the installed extension surface."""
import ast
import importlib
import inspect
from pathlib import Path
import pytest
import openswmm.engine


@pytest.mark.parametrize('module', ['_transport', '_groundwater', '_gw_transport', '_surface_quality'])
def test_new_view_stub_signatures(module):
    runtime = importlib.import_module('openswmm.engine.' + module)
    stub = Path(openswmm.engine.__file__).with_name(module + '.pyi')
    for cls in ast.parse(stub.read_text()).body:
        if not isinstance(cls, ast.ClassDef):
            continue
        actual = getattr(runtime, cls.name)
        for method in cls.body:
            if not isinstance(method, ast.FunctionDef):
                continue
            value = inspect.getattr_static(actual, method.name)
            if isinstance(value, property):
                assert any(isinstance(d, ast.Name) and d.id == 'property' or
                           isinstance(d, ast.Attribute) and d.attr == 'setter'
                           for d in method.decorator_list), (cls.name, method.name)
                continue
            signature = inspect.signature(getattr(actual, method.name))
            expected = [arg.arg for arg in method.args.posonlyargs + method.args.args + method.args.kwonlyargs]
            assert list(signature.parameters) == expected, (cls.name, method.name)
