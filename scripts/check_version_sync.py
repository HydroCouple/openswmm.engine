#!/usr/bin/env python3
"""Assert the package version agrees across CMake, vcpkg, and pyproject.

Single-source-of-truth *enforcement* (review finding #6): rather than derive
all three from one file (which would mean a fragile dynamic-version refactor of
the CMake + scikit-build-core build), we keep the three declarations and gate
them with this check in CI. They must normalize to the same (base, phase, num).
Python wheels may add a .devN suffix before that native release; both Python
packages and the companion dependency pin must share the exact full version.

Sources:
  - CMakeLists.txt          project(... VERSION X.Y.Z) + OPENSWMM_PRERELEASE
  - vcpkg.json              "version-semver": "X.Y.Z-<phase>.<n>"
  - python/pyproject.toml   [project] version = "X.Y.Z<a|b|rc><n>[.devN]"  (PEP 440)

CMakeLists.txt is the source: scripts/bump_version.py writes every entry of
DECLARATIONS from it, and this gate also checks the files that only restate
the version (RESTATED) and that each declaration is written exactly once.

Run: python scripts/check_version_sync.py   (exit 0 = in sync, 1 = mismatch)
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Map PEP 440 short phase <-> semver long phase.
_PHASE = {"a": "alpha", "b": "beta", "rc": "rc", "alpha": "alpha", "beta": "beta"}
_PEP440_PHASE = {"alpha": "a", "beta": "b", "rc": "rc"}

# Where the version is written: (file, pattern whose one group is the version text, spelling).
# Spellings: "base" X.Y.Z, "prerelease" alpha.N (empty for a final release), "semver"
# X.Y.Z-alpha.N, "python" the Python packages' exact version (PEP 440, X.Y.ZaN; a CI build may
# add .devN). Each pattern must match exactly once.
CORE = [
    ("CMakeLists.txt", r"project\(\s*openswmm\s+VERSION\s+([0-9.]+)", "base"),
    ("CMakeLists.txt", r'set\(OPENSWMM_PRERELEASE "([^"]*)"', "prerelease"),
    ("vcpkg.json", r'"version-semver"\s*:\s*"([^"]+)"', "semver"),
    ("python/pyproject.toml", r'^version\s*=\s*"([^"]+)"', "python"),
    ("packages/gpu-omp/pyproject.toml", r'^version\s*=\s*"([^"]+)"', "python"),
    ("packages/gpu-omp/pyproject.toml", r"openswmm\s*==\s*([^\"'\s]+)", "python"),
]
RESTATED = [
    ("CMakeLists.txt", r"^# Version:\s+(\S+) \(refactored engine\)", "semver"),
    ("python/CMakeLists.txt", r"project\(\s*openswmm-python\s+VERSION\s+([0-9.]+)", "base"),
    ("packages/gpu-omp/CMakeLists.txt", r"project\(openswmm-gpu-omp\s+VERSION\s+([0-9.]+)",
     "base"),
    ("docs/Doxyfile", r"^PROJECT_NUMBER\s*=\s*(\S+)", "semver"),
    ("docs/Doxyfile", r'^PROJECT_BRIEF\s*=\s*"[^"]*\(([^()"]+)\)"', "semver"),
    ("CITATION.cff", r'^version:\s*"([^"]+)"', "semver"),
    # The version the Python manual shows when the compiled package is not importable.
    ("python/docs/conf.py", r"^\s+version = '([^']+)'", "python"),
]
DECLARATIONS = CORE + RESTATED


def release_forms(base: str, phase: str, num: int) -> dict[str, str]:
    """A release's spellings, from (base, canonical phase or '', number)."""
    if not phase:
        return {"base": base, "prerelease": "", "semver": base, "pep440": base}
    return {"base": base, "prerelease": f"{phase}.{num}", "semver": f"{base}-{phase}.{num}",
            "pep440": f"{base}{_PEP440_PHASE[phase]}{num}"}


def read(path: Path) -> str:
    """A file's text with its line endings as they are."""
    with open(path, encoding="utf-8", newline="") as f:
        return f.read()


def write(path: Path, text: str) -> None:
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def find(text: str, name: str, pattern: str) -> re.Match:
    """The one match of a declaration's pattern; raises ValueError for none or several."""
    found = list(re.finditer(pattern, text, re.MULTILINE))
    if len(found) != 1:
        raise ValueError(f"{name}: expected one match of {pattern!r}, found {len(found)}")
    return found[0]


def replace(text: str, name: str, pattern: str, value: str) -> str:
    """The text with the declaration's version set to value."""
    m = find(text, name, pattern)
    return text[:m.start(1)] + value + text[m.end(1):]


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


def python_version(p: Path) -> str:
    """Read the complete Python release, including its development suffix."""
    return re.search(r'^version\s*=\s*"([^"]+)"', p.read_text(), re.MULTILINE).group(1)


def from_pyproject(p: Path) -> tuple[str, str, int]:
    """Match the native release while allowing a Python-only .devN suffix."""
    ver = python_version(p)
    m = re.fullmatch(r"([0-9]+\.[0-9]+\.[0-9]+)(?:(a|b|rc)([0-9]+))?(?:\.dev[0-9]+)?", ver)
    if m is None:
        raise ValueError(f"Unsupported Python package version in {p}: {ver}")
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
    companion_path = ROOT / "packages" / "gpu-omp" / "pyproject.toml"
    base_ver = python_version(ROOT / "python" / "pyproject.toml")
    companion_ver = python_version(companion_path)
    if companion_ver != base_ver:
        print(f"\nERROR: gpu-omp version {companion_ver} differs from base {base_ver}.",
              file=sys.stderr)
        return 1
    print(f"  Python packages                  -> {base_ver} (exact match)")
    companion = companion_path.read_text()
    pin = re.search(r"openswmm\s*==\s*([^\"'\s]+)", companion)
    if not pin or pin.group(1) != base_ver:
        print(f"\nERROR: gpu-omp companion pins 'openswmm == "
              f"{pin.group(1) if pin else '?'}' but base is {base_ver}.",
              file=sys.stderr)
        return 1
    print(f"  gpu-omp 'openswmm==' pin           -> {pin.group(1)} (matches base)")

    # Every declaration, the restatements included, holds the release once.
    forms = release_forms(*got["CMakeLists.txt"])
    forms["python"] = base_ver
    stale = []
    for name, pattern, form in DECLARATIONS:
        try:
            value = find(read(ROOT / name), name, pattern).group(1)
        except (OSError, ValueError) as exc:
            stale.append(str(exc))
            continue
        if value != forms[form]:
            stale.append(f"{name}: {value!r}, expected {forms[form]!r} ({form})")
    if stale:
        print("\nERROR: declarations out of step (scripts/bump_version.py rewrites them):\n  "
              + "\n  ".join(stale), file=sys.stderr)
        return 1
    print(f"  {len(DECLARATIONS)} declarations in {len({d[0] for d in DECLARATIONS})} files"
          "  -> each written once")

    print("\nOK: all version declarations agree.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
