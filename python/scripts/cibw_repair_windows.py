"""cibuildwheel repair-wheel hook for Windows.

Why this script exists
----------------------
cibuildwheel's default repair command is

    delvewheel repair -w {dest_dir} {wheel}

which only searches PATH for the wheel's DLL dependencies. That is not
enough for openswmm because:

  1. ``openswmm.engine.dll`` is installed INTO the wheel at
     ``openswmm/engine/openswmm.engine.dll`` (see
     ``python/openswmm/CMakeLists.txt``). delvewheel does not recursively
     scan wheel subdirectories for DLL search.
  2. Runtime deps installed by vcpkg (SUNDIALS, HDF5, sqlite3, …) live
     under ``%VCPKG_ROOT%/installed/x64-windows/bin``, which is not on
     PATH inside the cibuildwheel build venv.

What it does
------------
- Extracts the unrepaired wheel to a temp dir.
- Collects the directory of every ``*.dll`` found inside, recursively
  — that handles ``openswmm/engine/`` (and any future moves).
- Adds ``$VCPKG_ROOT/installed/<triplet>/bin`` if VCPKG_ROOT is set.
- Invokes ``delvewheel repair`` with all collected dirs as
  ``--add-path`` arguments.

Wired in from ``python/pyproject.toml``:

    [tool.cibuildwheel.windows]
    repair-wheel-command = "python python/scripts/cibw_repair_windows.py {wheel} {dest_dir}"

Note: cibuildwheel's ``repair-wheel-command`` only substitutes ``{wheel}``
and ``{dest_dir}`` (plus the default ``{python}``/``{pip}``). It does NOT
substitute ``{project}`` or ``{package}`` — those would be passed through
literally and break the command. Verified by reading cibuildwheel's source
(util.py::prepare_command and windows.py's call site). The script path is
therefore relative to cibuildwheel's cwd (workspace root, where the
repository was checked out), not the package dir.
"""
from __future__ import annotations

import os
import sys
import glob
import subprocess
import tempfile
import zipfile
from pathlib import Path


def _wheel_dll_dirs(wheel_path: Path, extract_to: Path) -> list[Path]:
    """Extract the wheel and return the dirs containing any .dll files."""
    with zipfile.ZipFile(wheel_path) as zf:
        zf.extractall(extract_to)
    dirs: set[Path] = set()
    for dll in extract_to.rglob("*.dll"):
        dirs.add(dll.parent.resolve())
    return sorted(dirs)


def _vcpkg_bin() -> Path | None:
    vcpkg_root = os.environ.get("VCPKG_ROOT")
    if not vcpkg_root:
        return None
    # x64-windows is the only Windows triplet we currently build for; if
    # we ever add ARM Windows or static triplets, parametrise this.
    triplet = os.environ.get("VCPKG_DEFAULT_TRIPLET", "x64-windows")
    candidate = Path(vcpkg_root) / "installed" / triplet / "bin"
    return candidate if candidate.is_dir() else None


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(f"usage: {argv[0]} <wheel> <dest_dir>", file=sys.stderr)
        return 2
    wheel = Path(argv[1]).resolve()
    dest = Path(argv[2]).resolve()
    dest.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="cibw_repair_") as td:
        add_paths = _wheel_dll_dirs(wheel, Path(td))
        vbin = _vcpkg_bin()
        if vbin is not None:
            add_paths.append(vbin)

        cmd: list[str] = ["delvewheel", "repair", "-w", str(dest)]
        for p in add_paths:
            cmd += ["--add-path", str(p)]
        cmd.append(str(wheel))

        print(">>> " + " ".join(cmd), flush=True)
        return subprocess.call(cmd)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
