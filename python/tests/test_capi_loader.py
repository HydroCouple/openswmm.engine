"""The raw C tests must use the same DLL as the installed extension on Windows."""
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from tests._capi import load_engine_api


class CApiLoaderTests(unittest.TestCase):
    extension = SimpleNamespace(__file__="/site-packages/openswmm/engine/_solver.pyd")

    def test_repaired_windows_wheel_prefers_the_imported_mangled_dll(self):
        mangled = Path("/site-packages/openswmm.libs/openswmm.engine-1234.dll")
        with patch("tests._capi.sys.platform", "win32"), \
                patch("tests._capi.Path.glob", return_value=iter([mangled])), \
                patch("tests._capi.ctypes.CDLL") as load:
            load_engine_api(self.extension)
            load.assert_called_once_with(str(mangled))

    def test_unrepaired_windows_build_loads_the_engine_dll(self):
        original = Path("/site-packages/openswmm/engine/openswmm.engine.dll")
        with patch("tests._capi.sys.platform", "win32"), \
                patch("tests._capi.Path.glob", side_effect=[iter([]), iter([original])]), \
                patch("tests._capi.ctypes.CDLL") as load:
            load_engine_api(self.extension)
            load.assert_called_once_with(str(original))

    def test_missing_windows_dll_fails_without_skipping_the_contract(self):
        with patch("tests._capi.sys.platform", "win32"), \
                patch("tests._capi.Path.glob", side_effect=[iter([]), iter([])]):
            with self.assertRaisesRegex(RuntimeError, "Expected one engine DLL"):
                load_engine_api(self.extension)

    def test_unix_resolves_symbols_from_the_extension(self):
        with patch("tests._capi.sys.platform", "linux"), \
                patch("tests._capi.ctypes.CDLL") as load:
            load_engine_api(self.extension)
            load.assert_called_once_with(self.extension.__file__)
