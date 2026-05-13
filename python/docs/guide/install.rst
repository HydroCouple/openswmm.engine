============
Installation
============

The :mod:`openswmm` package ships pre-built **wheels** for the platforms
listed below.  When a wheel is available for your interpreter, ``pip
install`` finishes in seconds with no compiler toolchain on your machine.

Supported platforms
===================

.. list-table::
   :header-rows: 1
   :widths: 25 25 50

   * - Platform
     - Architectures
     - Python versions
   * - macOS
     - arm64 (Apple Silicon), x86_64 (Intel)
     - 3.9 – 3.13
   * - Linux  (manylinux 2_28)
     - x86_64, aarch64
     - 3.9 – 3.13
   * - Windows
     - x86_64
     - 3.9 – 3.13

Quick install
=============

In a virtual environment (any flavour — ``venv``, ``virtualenv``,
``conda``, ``mamba``)::

    pip install openswmm

That's it.  Verify::

    python -c "import openswmm; print(openswmm.__version__)"

Conda / Mamba environments
==========================

A conda environment is treated like any other venv: the wheel ships
its own libomp / vcpkg-bundled libraries, so conda's own
system libraries are not used or required.  Inside an active environment::

    conda create -n stormwater python=3.13 numpy
    conda activate stormwater
    pip install openswmm

The :mod:`openswmm` package itself is **not** distributed via
``conda-forge`` at this time; install via ``pip`` inside the conda env.

Verifying the install
=====================

A complete sanity check that exercises every Cython extension::

    python -c "
    import openswmm, importlib
    import openswmm.engine as e
    mods = ['_solver','_model','_edit','_nodes','_links','_subcatchments',
            '_gages','_hotstart','_massbalance','_pollutants','_tables',
            '_inflows','_controls','_infrastructure','_quality','_statistics',
            '_output_reader','_spatial','_forcing']
    for m in mods:
        importlib.import_module('openswmm.engine.' + m)
    print(f'openswmm {openswmm.__version__}: all {len(mods)} modules OK')
    "

Building from source
====================

Skip this section unless you are developing the engine itself, building
for an unsupported architecture, or contributing patches.

Prerequisites
-------------

* CMake ≥ 3.24
* Ninja (or Make / Visual Studio on Windows)
* A C++20 compiler (Apple clang, GCC ≥ 11, MSVC v143)
* Python ≥ 3.9 with the headers (``python3-dev`` on Debian/Ubuntu)
* `vcpkg <https://github.com/microsoft/vcpkg>`_ checked out and the
  ``VCPKG_ROOT`` environment variable set

Optional, platform-specific:

* macOS: ``brew install libomp ninja``
* Linux: ``apt install ninja-build libgomp1``  *(or the yum equivalent)*
* Windows: install MSVC and Ninja via Visual Studio Installer

Editable / development install
------------------------------

::

    git clone --recursive https://github.com/HydroCouple/openswmm.engine.git
    cd openswmm.engine/python
    python -m venv .venv
    source .venv/bin/activate            # Windows: .venv\Scripts\activate
    pip install -U pip
    pip install "scikit-build-core>=0.10" "cython>=3.0.12" "numpy>=1.21" cmake ninja
    pip install -e . --no-build-isolation

The first build compiles the full C++ engine; subsequent rebuilds reuse
the CMake cache and finish in seconds.

.. warning::

   Always pass ``--no-build-isolation`` for editable installs.

   Without it, ``pip`` creates an ephemeral build env, installs CMake
   into it, runs the configure step, and then deletes the env once
   the install completes.  ``build.ninja`` ends up with the absolute
   path to the deleted CMake baked in, so any subsequent rebuild fails
   with::

       /bin/sh: /private/var/folders/.../pip-build-env-XXXX/.../cmake:
       No such file or directory

   The same trap applies to ``conda`` environments: pip's build
   isolation uses its own ephemeral env regardless of conda.

After editing a ``.pyx`` file, re-run the same command to rebuild:

::

    pip install -e . --no-build-isolation

(Auto-rebuild on import is **off** by design — see ``editable.rebuild``
in [python/pyproject.toml](python/pyproject.toml) for the rationale.)

Build a release wheel
---------------------

::

    cd python
    python -m build --wheel --no-isolation -o dist
    # → dist/openswmm-6.0.0.dev0-cp313-cp313-macosx_26_0_arm64.whl

The wheel bundles all engine shared libraries and is fully
self-contained; you can ship it as-is.

Cross-platform wheels (CI)
--------------------------

::

    cd python
    cibuildwheel --output-dir wheelhouse

Wheels for every supported (platform × Python) combination land in
``wheelhouse/``.

Cleaning a stale build
----------------------

If a partial build leaves stale ``.so`` / ``.dylib`` files in the source
tree (a common cause of "imports succeed but every other call fails"),
nuke them with::

    cd python
    ./scripts/clean.sh

Environment variables
---------------------

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Variable
     - Purpose
   * - ``VCPKG_ROOT``
     - Path to a vcpkg checkout. When set, the vcpkg toolchain file is
       loaded automatically.
   * - ``OPENSWMM_ENGINE_INSTALL_PREFIX``
     - Path to a pre-built OpenSWMM engine install. Skips building the
       engine from source — used in CI to avoid redundant rebuilds.
   * - ``DEBUG=1``
     - Switch ``CMAKE_BUILD_TYPE`` to ``Debug`` (debug symbols, no
       optimisation).
   * - ``CMAKE_ARGS``
     - Extra ``-D…`` flags forwarded to every CMake configure.
   * - ``SKBUILD_CMAKE_ARGS``
     - scikit-build-core's own CMake-flag override channel.

Example — link against a pre-built engine::

    export VCPKG_ROOT=/path/to/vcpkg
    export OPENSWMM_ENGINE_INSTALL_PREFIX=/path/to/engine/install
    pip install . --no-build-isolation

Example — enable optional modules::

    export CMAKE_ARGS="-DOPENSWMM_BUILD_2D=ON -DOPENSWMM_WITH_GEOPACKAGE=ON"
    pip install -e . --no-build-isolation
