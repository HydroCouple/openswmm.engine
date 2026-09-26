"""Run pytest against a staged package using the active Python environment.

Usage: conda run -n openswmm python python/scripts/test_staged_package.py STAGE [pytest args]

Scikit-build editable imports override PYTHONPATH. Remove only OpenSWMM's
redirector for this process, and assert the loaded native modules belong to
STAGE. This does not modify the installed package or environment.
"""
from pathlib import Path
import sys


def main():
    stage = Path(sys.argv[1]).resolve()
    source = Path(__file__).resolve().parents[1]
    sys.meta_path[:] = [finder for finder in sys.meta_path
                       if type(finder).__module__ != "_openswmm_editable"]
    sys.path[:0] = [str(stage), str(source)]
    import openswmm.engine._solver as solver
    assert Path(solver.__file__).is_relative_to(stage), solver.__file__
    for name, module in tuple(sys.modules.items()):
        if name.startswith("openswmm.") and getattr(module, "__file__", None):
            assert Path(module.__file__).is_relative_to(stage), (name, module.__file__)
    print(f"Python: {sys.executable}\nBindings: {solver.__file__}", flush=True)
    import pytest
    return pytest.main(["--import-mode=importlib", *sys.argv[2:]])


if __name__ == "__main__":
    raise SystemExit(main())
