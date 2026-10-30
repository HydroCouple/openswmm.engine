"""Load the engine C API used by an installed Cython extension."""
import ctypes
from pathlib import Path
import sys


def load_engine_api(extension):
    if sys.platform != "win32":
        return ctypes.CDLL(extension.__file__)
    # GetProcAddress on a .pyd does not search its imported DLLs. Repaired
    # wheels import the mangled DLL in openswmm.libs; loading the original
    # in-package copy would create a second engine instance in the process.
    engine_dir = Path(extension.__file__).resolve().parent
    candidates = list((engine_dir.parent.parent / "openswmm.libs").glob("openswmm.engine-*.dll"))
    if not candidates:
        candidates = list(engine_dir.glob("openswmm.engine.dll"))
    if len(candidates) != 1:
        raise RuntimeError(f"Expected one engine DLL for {extension.__file__}, found {candidates}")
    return ctypes.CDLL(str(candidates[0]))
