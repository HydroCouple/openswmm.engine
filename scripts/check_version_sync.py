#!/usr/bin/env python3
"""Assert the package version agrees across CMake, vcpkg, and pyproject.

Single-source-of-truth *enforcement* (review finding #6): rather than derive
all three from one file (which would mean a fragile dynamic-version refactor of
the CMake + scikit-build-core build), we keep the three declarations and gate
them with this check in CI. They must normalize to the same (base, phase, num).

Sources:
  - CMakeLists.txt          project(... VERSION X.Y.Z) + OPENSWMM_PRERELEASE
  - vcpkg.json              "version-semver": "X.Y.Z-<phase>.<n>"
  - python/pyproject.toml   [project] version = "X.Y.Z<a|b|rc><n>"  (PEP 440)

Run: python scripts/check_version_sync.py   (exit 0 = in sync, 1 = mismatch)
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Map PEP 440 short phase <-> semver long phase.
_PHASE = {"a": "alpha", "b": "beta", "rc": "rc", "alpha": "alpha", "beta": "beta"}


def _norm(base: str, phase: str | None, num: int | None) -> tuple[str, str, int]:
    """Normalize to (base, canonical-phase, number). Final release -> ('', 0)."""
    if phase is None:
        return (base, "", 0)
    return (base, _PHASE.get(phase, phase), int(num or 0))


def from_cmake(p: Path) -> tuple[str, str, int]:
    txt = p.read_text()
    base = re.search(r"project\(\s*openswmm\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)",
                     txt, re.IGNORECASE).group(1)
    m = re.search(r'OPENSWMM_PRERELEASE\s+"([^"]*)"', txt)
    pre = m.group(1) if m else ""
    if not pre:
        return _norm(base, None, None)
    phase, num = re.match(r"([a-zA-Z]+)\.?([0-9]+)", pre).groups()
    return _norm(base, phase, num)


def from_vcpkg(p: Path) -> tuple[str, str, int]:
    ver = re.search(r'"version-semver"\s*:\s*"([^"]+)"', p.read_text()).group(1)
    m = re.match(r"([0-9]+\.[0-9]+\.[0-9]+)(?:-([a-zA-Z]+)\.?([0-9]+))?$", ver)
    base, phase, num = m.groups()
    return _norm(base, phase, num)


def from_pyproject(p: Path) -> tuple[str, str, int]:
    ver = re.search(r'^version\s*=\s*"([^"]+)"', p.read_text(), re.MULTILINE).group(1)
    m = re.match(r"([0-9]+\.[0-9]+\.[0-9]+)(?:(a|b|rc)([0-9]+))?$", ver)
    base, phase, num = m.groups()
    return _norm(base, phase, num)


def main() -> int:
    got = {
        "CMakeLists.txt":       from_cmake(ROOT / "CMakeLists.txt"),
        "vcpkg.json":           from_vcpkg(ROOT / "vcpkg.json"),
        "python/pyproject.toml": from_pyproject(ROOT / "python" / "pyproject.toml"),
        "packages/gpu-omp/pyproject.toml":
            from_pyproject(ROOT / "packages" / "gpu-omp" / "pyproject.toml"),
    }
    for name, v in got.items():
        print(f"  {name:32s} -> {v[0]}-{v[1] or 'final'}.{v[2]}")
    distinct = set(got.values())
    if len(distinct) != 1:
        print("\nERROR: version mismatch across declarations.", file=sys.stderr)
        return 1

    # The GPU-omp companion is version-locked to the base engine at runtime via
    # its `openswmm == <ver>` dependency pin; that pin must track the shared
    # version too (the pyproject `version =` check above only covers the
    # companion's OWN version).
    companion = (ROOT / "packages" / "gpu-omp" / "pyproject.toml").read_text()
    pin = re.search(r'openswmm\s*==\s*([^"\'\s]+)', companion)
    base_ver = re.search(r'^version\s*=\s*"([^"]+)"',
                         (ROOT / "python" / "pyproject.toml").read_text(),
                         re.MULTILINE).group(1)
    if not pin or pin.group(1) != base_ver:
        print(f"\nERROR: gpu-omp companion pins 'openswmm == "
              f"{pin.group(1) if pin else '?'}' but base is {base_ver}.",
              file=sys.stderr)
        return 1
    print(f"  gpu-omp 'openswmm==' pin           -> {pin.group(1)} (matches base)")

    print("\nOK: all version declarations agree.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
