"""Run delvewheel on every wheel in dist/ with the right --add-path hints.

delvewheel needs to locate the engine DLL (openswmm.engine.dll) and its
transitive runtime dependencies (vcpkg-installed libs, OpenMP, etc.) so it
can bundle them into the repaired wheel. By default delvewheel only looks
at PATH and the wheel's own directories, which is not enough here: the
runtime deps live under VCPKG_ROOT/installed/<triplet>/bin, and the engine
DLL itself is most reliably found via the already-installed package from
the preceding `pip install .` step.
"""
from __future__ import annotations

import glob
import os
import subprocess
import sys
from pathlib import Path


def _site_packages_openswmm() -> Path | None:
    # -P would be cleaner but we're already running with the source tree
    # absent from sys.path (this script is invoked from python/, not from
    # python/openswmm/). Import the installed package and use its __path__.
    try:
        import openswmm  # type: ignore
    except ImportError:
        return None
    return Path(openswmm.__path__[0])


def _vcpkg_bin() -> Path | None:
    vcpkg_root = os.environ.get("VCPKG_ROOT")
    triplet = os.environ.get("VCPKG_DEFAULT_TRIPLET", "x64-windows")
    if not vcpkg_root:
        return None
    candidate = Path(vcpkg_root) / "installed" / triplet / "bin"
    return candidate if candidate.is_dir() else None


def collect_add_paths() -> list[str]:
    paths: set[str] = set()

    pkg_root = _site_packages_openswmm()
    if pkg_root is not None:
        for dll in pkg_root.rglob("*.dll"):
            paths.add(str(dll.parent))

    vcpkg_bin = _vcpkg_bin()
    if vcpkg_bin is not None:
        paths.add(str(vcpkg_bin))

    return sorted(paths)


def main() -> int:
    wheels = glob.glob("dist/*.whl")
    if not wheels:
        print("No wheel found in dist/", file=sys.stderr)
        return 1

    add_paths = collect_add_paths()
    print("delvewheel --add-path entries:")
    for p in add_paths:
        print(f"  {p}")

    base_cmd = [sys.executable, "-m", "delvewheel", "repair", "-w", "dist"]
    for p in add_paths:
        base_cmd += ["--add-path", p]

    for wheel in wheels:
        subprocess.check_call(base_cmd + [wheel])
    return 0


if __name__ == "__main__":
    sys.exit(main())
