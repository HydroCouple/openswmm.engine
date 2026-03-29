"""scikit-build entry point for the openswmm Python package.

This file exists because scikit-build (< 1.0) requires a setup.py that calls
``skbuild.setup()``.  All declarative metadata lives in ``pyproject.toml``;
only the CMake integration arguments are set here.

Build
-----
    pip install .                           # standard install
    pip install -e . --no-build-isolation   # editable (dev)
    DEBUG=1 pip install .                   # debug build
"""

import os
import platform
import shutil

from skbuild import setup
from setuptools import find_packages

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
_HERE = os.path.abspath(os.path.dirname(__file__))
_DEBUG = os.getenv("DEBUG", "0").lower() in ("true", "1", "t")
_PLATFORM = platform.system()  # "Windows", "Linux", or "Darwin"


def _cmake_preset() -> str:
    """Return the CMakePresets.json preset name for the current platform."""
    suffix = "-debug" if _DEBUG else ""
    return f"{_PLATFORM}{suffix}"


def _ensure_readme() -> str:
    """Copy the project README into the sdist if it isn't here yet."""
    local = os.path.join(_HERE, "README.md")
    if not os.path.exists(local):
        parent = os.path.join(_HERE, os.pardir, "README.md")
        if os.path.exists(parent):
            shutil.copyfile(parent, local)
    if os.path.exists(local):
        with open(local, encoding="utf-8") as f:
            return f.read()
    return ""


# ---------------------------------------------------------------------------
# scikit-build setup
# ---------------------------------------------------------------------------
setup(
    packages=find_packages(exclude=["tests", "tests.*"]),
    long_description=_ensure_readme(),
    long_description_content_type="text/markdown",
    cmake_args=[
        f"--preset={_cmake_preset()}",
        *([f"-DCMAKE_OSX_ARCHITECTURES={os.environ['CMAKE_OSX_ARCHITECTURES']}"]
          if "CMAKE_OSX_ARCHITECTURES" in os.environ else []),
        *os.getenv("OPENSWMM_CMAKE_ARGS", "").split(),
    ],
    include_package_data=True,
)
